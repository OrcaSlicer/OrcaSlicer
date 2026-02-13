#include "ClipperUtils.hpp"
#include "Model.hpp"
#include "Print.hpp"

#include <boost/log/trivial.hpp>
#include <cfloat>

namespace Slic3r {

// Add or remove support modifier ModelVolumes from model_object_dst to match the ModelVolumes of model_object_new
// in the exact order and with the same IDs.
// It is expected, that the model_object_dst already contains the non-support volumes of model_object_new in the correct order.
// Friend to ModelVolume to allow copying.
// static is not accepted by gcc if declared as a friend of ModelObject.
/* static */ void model_volume_list_update_supports(ModelObject &model_object_dst, const ModelObject &model_object_new)
{
    typedef std::pair<const ModelVolume*, bool> ModelVolumeWithStatus;
    std::vector<ModelVolumeWithStatus> old_volumes;
    old_volumes.reserve(model_object_dst.volumes.size());
    for (const ModelVolume *model_volume : model_object_dst.volumes)
        old_volumes.emplace_back(ModelVolumeWithStatus(model_volume, false));
    auto model_volume_lower = [](const ModelVolumeWithStatus &mv1, const ModelVolumeWithStatus &mv2){ return mv1.first->id() <  mv2.first->id(); };
    auto model_volume_equal = [](const ModelVolumeWithStatus &mv1, const ModelVolumeWithStatus &mv2){ return mv1.first->id() == mv2.first->id(); };
    std::sort(old_volumes.begin(), old_volumes.end(), model_volume_lower);
    model_object_dst.volumes.clear();
    model_object_dst.volumes.reserve(model_object_new.volumes.size());
    for (const ModelVolume *model_volume_src : model_object_new.volumes) {
        ModelVolumeWithStatus key(model_volume_src, false);
        auto it = std::lower_bound(old_volumes.begin(), old_volumes.end(), key, model_volume_lower);
        if (it != old_volumes.end() && model_volume_equal(*it, key)) {
            // The volume was found in the old list. Just copy it.
            assert(! it->second); // not consumed yet
            it->second = true;
            ModelVolume *model_volume_dst = const_cast<ModelVolume*>(it->first);
            // For support modifiers, the type may have been switched from blocker to enforcer and vice versa.
            assert((model_volume_dst->is_support_modifier() && model_volume_src->is_support_modifier()) || model_volume_dst->type() == model_volume_src->type());
            model_object_dst.volumes.emplace_back(model_volume_dst);
            if (model_volume_dst->is_support_modifier()) {
                // For support modifiers, the type may have been switched from blocker to enforcer and vice versa.
                model_volume_dst->set_type(model_volume_src->type());
                model_volume_dst->set_transformation(model_volume_src->get_transformation());
            }
            assert(model_volume_dst->get_matrix().isApprox(model_volume_src->get_matrix()));
        } else {
            // The volume was not found in the old list. Create a new copy.
            assert(model_volume_src->is_support_modifier());
            model_object_dst.volumes.emplace_back(new ModelVolume(*model_volume_src));
            model_object_dst.volumes.back()->set_model_object(&model_object_dst);
        }
    }
    // Release the non-consumed old volumes (those were deleted from the new list).
    for (ModelVolumeWithStatus &mv_with_status : old_volumes)
        if (! mv_with_status.second)
            delete mv_with_status.first;
}

static inline void model_volume_list_copy_configs(ModelObject &model_object_dst, const ModelObject &model_object_src, const ModelVolumeType type)
{
    size_t i_src, i_dst;
    for (i_src = 0, i_dst = 0; i_src < model_object_src.volumes.size() && i_dst < model_object_dst.volumes.size();) {
        const ModelVolume &mv_src = *model_object_src.volumes[i_src];
        ModelVolume       &mv_dst = *model_object_dst.volumes[i_dst];
        if (mv_src.type() != type) {
            ++ i_src;
            continue;
        }
        if (mv_dst.type() != type) {
            ++ i_dst;
            continue;
        }
        assert(mv_src.id() == mv_dst.id());
        // Copy the ModelVolume data.
        mv_dst.name   = mv_src.name;
		mv_dst.config.assign_config(mv_src.config);
        assert(mv_dst.supported_facets.id() == mv_src.supported_facets.id());
        mv_dst.supported_facets.assign(mv_src.supported_facets);
        assert(mv_dst.seam_facets.id() == mv_src.seam_facets.id());
        mv_dst.seam_facets.assign(mv_src.seam_facets);
        assert(mv_dst.mmu_segmentation_facets.id() == mv_src.mmu_segmentation_facets.id());
        mv_dst.mmu_segmentation_facets.assign(mv_src.mmu_segmentation_facets);
        assert(mv_dst.fuzzy_skin_facets.id() == mv_src.fuzzy_skin_facets.id());
        mv_dst.fuzzy_skin_facets.assign(mv_src.fuzzy_skin_facets);
        //FIXME what to do with the materials?
        // mv_dst.m_material_id = mv_src.m_material_id;
        ++ i_src;
        ++ i_dst;
    }
}

static inline void layer_height_ranges_copy_configs(t_layer_config_ranges &lr_dst, const t_layer_config_ranges &lr_src)
{
    assert(lr_dst.size() == lr_src.size());
    auto it_src = lr_src.cbegin();
    for (auto &kvp_dst : lr_dst) {
        const auto &kvp_src = *it_src ++;
        assert(std::abs(kvp_dst.first.first  - kvp_src.first.first ) <= EPSILON);
        assert(std::abs(kvp_dst.first.second - kvp_src.first.second) <= EPSILON);
        // Layer heights are allowed do differ in case the layer height table is being overriden by the smooth profile.
        // assert(std::abs(kvp_dst.second.option("layer_height")->getFloat() - kvp_src.second.option("layer_height")->getFloat()) <= EPSILON);
        kvp_dst.second = kvp_src.second;
    }
}

static inline bool transform3d_lower(const Transform3d &lhs, const Transform3d &rhs)
{
    typedef Transform3d::Scalar T;
    const T *lv = lhs.data();
    const T *rv = rhs.data();
    for (size_t i = 0; i < 16; ++ i, ++ lv, ++ rv) {
        if (*lv < *rv)
            return true;
        else if (*lv > *rv)
            return false;
    }
    return false;
}

static inline bool transform3d_equal(const Transform3d &lhs, const Transform3d &rhs)
{
    typedef Transform3d::Scalar T;
    const T *lv = lhs.data();
    const T *rv = rhs.data();
    for (size_t i = 0; i < 16; ++ i, ++ lv, ++ rv)
        if (*lv != *rv)
            return false;
    return true;
}

struct PrintObjectTrafoAndInstances
{
    Transform3d    	trafo;
    PrintInstances	instances;
    bool operator<(const PrintObjectTrafoAndInstances &rhs) const { return transform3d_lower(this->trafo, rhs.trafo); }
};

// Generate a list of trafos and XY offsets for instances of a ModelObject
// Orca: Updated to include XYZ filament shrinkage compensation
static std::vector<PrintObjectTrafoAndInstances> print_objects_from_model_object(const ModelObject &model_object, const Vec3d &shrinkage_compensation)
{
    std::set<PrintObjectTrafoAndInstances> trafos;
    PrintObjectTrafoAndInstances           trafo;
    //BBS: add useful logs for debug
    int index = 0;
    for (ModelInstance *model_instance : model_object.instances) {
        if (model_instance->is_printable()) {
            // Orca: Updated with XYZ filament shrinkage compensation
            Geometry::Transformation model_instance_transformation = model_instance->get_transformation();
            trafo.trafo = model_instance_transformation.get_matrix_with_applied_shrinkage_compensation(shrinkage_compensation);
            
            auto shift = Point::new_scale(trafo.trafo.data()[12], trafo.trafo.data()[13]);
            // Reset the XY axes of the transformation.
            trafo.trafo.data()[12] = 0;
            trafo.trafo.data()[13] = 0;
            // Search or insert a trafo.
            auto it = trafos.emplace(trafo).first;
            const_cast<PrintObjectTrafoAndInstances&>(*it).instances.emplace_back(PrintInstance{ nullptr, model_instance, shift });
            //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", Line %1%: found object %2%'s instance %3% for print")%__LINE__ %model_object.name %index;
        }
        else {
            //BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", Line %1%: found object %2%'s instance %3% not printable")%__LINE__ %model_object.name %index;
            //BOOST_LOG_TRIVIAL(debug) << boost::format(" object printable %1%, instance printable %2%, print_volume_state %3%")%model_object.printable %model_instance->printable %model_instance->print_volume_state;
        }
        index++;
    }

    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: got %2% print objects")%__LINE__ %trafos.size();
    return std::vector<PrintObjectTrafoAndInstances>(trafos.begin(), trafos.end());
}

// Compare just the layer ranges and their layer heights, not the associated configs.
// Ignore the layer heights if check_layer_heights is false.
static bool layer_height_ranges_equal(const t_layer_config_ranges &lr1, const t_layer_config_ranges &lr2, bool check_layer_height)
{
    if (lr1.size() != lr2.size())
        return false;
    auto it2 = lr2.begin();
    for (const auto &kvp1 : lr1) {
        const auto &kvp2 = *it2 ++;
        if (!kvp2.second.has("layer_height") || !kvp1.second.has("layer_height"))
            return false;
        if (std::abs(kvp1.first.first  - kvp2.first.first ) > EPSILON ||
            std::abs(kvp1.first.second - kvp2.first.second) > EPSILON ||
            (check_layer_height && std::abs(kvp1.second.option("layer_height")->getFloat() - kvp2.second.option("layer_height")->getFloat()) > EPSILON))
            return false;
    }
    return true;
}

// Returns true if va == vb when all CustomGCode items that are not ToolChangeCode are ignored.
static bool custom_per_printz_gcodes_tool_changes_differ(const std::vector<CustomGCode::Item> &va, const std::vector<CustomGCode::Item> &vb)
{
	auto it_a = va.begin();
	auto it_b = vb.begin();
	while (it_a != va.end() || it_b != vb.end()) {
		if (it_a != va.end() && it_a->type != CustomGCode::ToolChange) {
			// Skip any CustomGCode items, which are not tool changes.
			++ it_a;
			continue;
		}
		if (it_b != vb.end() && it_b->type != CustomGCode::ToolChange) {
			// Skip any CustomGCode items, which are not tool changes.
			++ it_b;
			continue;
		}
		if (it_a == va.end() || it_b == vb.end())
			// va or vb contains more Tool Changes than the other.
			return true;
		assert(it_a->type == CustomGCode::ToolChange);
		assert(it_b->type == CustomGCode::ToolChange);
		if (*it_a != *it_b)
			// The two Tool Changes differ.
			return true;
		++ it_a;
		++ it_b;
	}
	// There is no change in custom Tool Changes.
	return false;
}

// Collect changes to print config, account for overrides of extruder retract values by filament presets.
//BBS: add plate index
static t_config_option_keys print_config_diffs(
    const PrintConfig        &current_config,
    const DynamicPrintConfig &new_full_config,
    DynamicPrintConfig       &filament_overrides,
    int                      plate_index,
    std::vector<int>&        filament_maps)
{
    const std::vector<std::string> &extruder_retract_keys = print_config_def.extruder_retract_keys();
    const std::string               filament_prefix       = "filament_";
    t_config_option_keys            print_diff;
    for (const t_config_option_key &opt_key : current_config.keys()) {
        const ConfigOption *opt_old = current_config.option(opt_key);
        assert(opt_old != nullptr);
        const ConfigOption *opt_new = new_full_config.option(opt_key);
        // assert(opt_new != nullptr);
        if (opt_new == nullptr)
            //FIXME This may happen when executing some test cases.
            continue;
        const ConfigOption *opt_new_filament = std::binary_search(extruder_retract_keys.begin(), extruder_retract_keys.end(), opt_key) ? new_full_config.option(filament_prefix + opt_key) : nullptr;

        if (opt_new_filament != nullptr) {
            compute_filament_override_value(opt_key, opt_old, opt_new, opt_new_filament, new_full_config, print_diff, filament_overrides, filament_maps);
        } else if (*opt_new != *opt_old) {
            //BBS: add plate_index logic for wipe_tower_x/wipe_tower_y
            if (!opt_key.compare("wipe_tower_x") || !opt_key.compare("wipe_tower_y")) {
                const ConfigOptionFloats* option_new = dynamic_cast<const ConfigOptionFloats*>(opt_new);
                const ConfigOptionFloats* option_old = dynamic_cast<const ConfigOptionFloats*>(opt_old);
                if ((plate_index < option_new->values.size())&&(plate_index < option_old->values.size()))
                {
                    float value_new = option_new->values[plate_index];
                    float value_old = option_old->values[plate_index];
                    if (value_old != value_new)
                        print_diff.emplace_back(opt_key);
                }
                else if ((plate_index < option_new->values.size())||(plate_index < option_old->values.size()))
                    print_diff.emplace_back(opt_key);
            }
            else
                print_diff.emplace_back(opt_key);
        }
    }

    return print_diff;
}

// Prepare for storing of the full print config into new_full_config to be exported into the G-code and to be used by the PlaceholderParser.
//BBS: add plate index
static t_config_option_keys full_print_config_diffs(const DynamicPrintConfig &current_full_config, const DynamicPrintConfig &new_full_config, int plate_index)
{
    t_config_option_keys full_config_diff;
    for (const t_config_option_key &opt_key : new_full_config.keys()) {
        const ConfigOption *opt_old = current_full_config.option(opt_key);
        const ConfigOption *opt_new = new_full_config.option(opt_key);
        if (opt_old == nullptr || *opt_new != *opt_old) {
            //BBS: add plate_index logic for wipe_tower_x/wipe_tower_y
            if (opt_old && (!opt_key.compare("wipe_tower_x") || !opt_key.compare("wipe_tower_y"))) {
                const ConfigOptionFloats* option_new = dynamic_cast<const ConfigOptionFloats*>(opt_new);
                const ConfigOptionFloats* option_old = dynamic_cast<const ConfigOptionFloats*>(opt_old);
                if ((plate_index < option_new->values.size())&&(plate_index < option_old->values.size()))
                {
                    float value_new = option_new->values[plate_index];
                    float value_old = option_old->values[plate_index];
                    if (value_old != value_new)
                        full_config_diff.emplace_back(opt_key);
                }
                else if ((plate_index < option_new->values.size())||(plate_index < option_old->values.size()))
                    full_config_diff.emplace_back(opt_key);
            }
            else
                full_config_diff.emplace_back(opt_key);
        }
    }
    return full_config_diff;
}

static bool is_printable_filament_changed(const DynamicPrintConfig& new_full_config, const Polygon& old_poly, const Polygon& new_poly)
{
    if (old_poly != new_poly) {
        auto map_mode_opt = new_full_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode");
        if (map_mode_opt && map_mode_opt->value == FilamentMapMode::fmmManual)
            return false;

        Pointfs              printable_area = new_full_config.option<ConfigOptionPoints>("printable_area")->values;
        std::vector<Pointfs> extruder_areas = new_full_config.option<ConfigOptionPointsGroups>("extruder_printable_area")->values;
        Points               pts;
        for (auto pt : printable_area) { pts.emplace_back(Point(scale_(pt.x()), scale_(pt.y()))); }
        Polygon printable_poly(pts);

        Polygons extruder_polys;
        for (auto extruder_area : extruder_areas) {
            pts.clear();
            for (auto pt : extruder_area) { pts.emplace_back(Point(scale_(pt.x()), scale_(pt.y()))); }
            extruder_polys.emplace_back(Polygon(pts));
        }

        Polygons split_polys;
        for (const Polygon& poly : extruder_polys) {
            Polygons res = diff(printable_poly, poly);
            if (!res.empty()) { split_polys.emplace_back(res[0]); }
        }

        Polygons all_extruder_polys = intersection({printable_poly}, extruder_polys);
        if (!all_extruder_polys.empty()) split_polys.emplace_back(all_extruder_polys[0]);

        auto find_intersections = [](const Polygon &poly, const Polygons &contours) -> std::set<int> {
            std::set<int> result;
            for (size_t i = 0; i < contours.size(); ++i) {
                if (!intersection(poly, contours[i]).empty()) { result.insert(static_cast<int>(i)); }
            }
            return result;
        };

        std::set<int> old_poly_ids = find_intersections(old_poly, split_polys);
        std::set<int> new_poly_ids = find_intersections(new_poly, split_polys);

        return old_poly_ids != new_poly_ids;
    }
    return false;
}

// Repository for solving partial overlaps of ModelObject::layer_config_ranges.
// Here the const DynamicPrintConfig* point to the config in ModelObject::layer_config_ranges.
class LayerRanges
{
public:
    struct LayerRange {
        t_layer_height_range        layer_height_range;
        // Config is owned by the associated ModelObject.
        const DynamicPrintConfig*   config { nullptr };

        bool operator<(const LayerRange &rhs) const throw() { return this->layer_height_range < rhs.layer_height_range; }
    };

    LayerRanges() = default;
    LayerRanges(const t_layer_config_ranges &in) { this->assign(in); }

    // Convert input config ranges into continuous non-overlapping sorted vector of intervals and their configs.
    void assign(const t_layer_config_ranges &in) {
        m_ranges.clear();
        m_ranges.reserve(in.size());
        // Input ranges are sorted lexicographically. First range trims the other ranges.
        coordf_t last_z = 0;
        for (const std::pair<const t_layer_height_range, ModelConfig> &range : in)
            if (range.first.second > last_z) {
                coordf_t min_z = std::max(range.first.first, 0.);
                if (min_z > last_z + EPSILON) {
                    m_ranges.push_back({ t_layer_height_range(last_z, min_z) });
                    last_z = min_z;
                }
                if (range.first.second > last_z + EPSILON) {
                    const DynamicPrintConfig *cfg = &range.second.get();
                    m_ranges.push_back({ t_layer_height_range(last_z, range.first.second), cfg });
                    last_z = range.first.second;
                }
            }
        if (m_ranges.empty())
            m_ranges.push_back({ t_layer_height_range(0, DBL_MAX) });
        else if (m_ranges.back().config == nullptr)
            m_ranges.back().layer_height_range.second = DBL_MAX;
        else
            m_ranges.push_back({ t_layer_height_range(m_ranges.back().layer_height_range.second, DBL_MAX) });
    }

    const DynamicPrintConfig* config(const t_layer_height_range &range) const {
        auto it = std::lower_bound(m_ranges.begin(), m_ranges.end(), LayerRange{ { range.first - EPSILON, range.second - EPSILON } });
        // #ys_FIXME_COLOR
        // assert(it != m_ranges.end());
        // assert(it == m_ranges.end() || std::abs(it->first.first  - range.first ) < EPSILON);
        // assert(it == m_ranges.end() || std::abs(it->first.second - range.second) < EPSILON);
        if (it == m_ranges.end() ||
            std::abs(it->layer_height_range.first - range.first) > EPSILON ||
            std::abs(it->layer_height_range.second - range.second) > EPSILON )
            return nullptr; // desired range doesn't found
        return it == m_ranges.end() ? nullptr : it->config;
    }

    std::vector<LayerRange>::const_iterator begin() const { return m_ranges.cbegin(); }
    std::vector<LayerRange>::const_iterator end  () const { return m_ranges.cend(); }
    size_t                                  size () const { return m_ranges.size(); }

private:
    // Layer ranges with their config overrides and list of volumes with their snug bounding boxes in a given layer range.
    std::vector<LayerRange>  m_ranges;
};

// To track Model / ModelObject updates between the front end and back end, including layer height ranges, their configs,
// and snug bounding boxes of ModelVolumes.
struct ModelObjectStatus {
    enum Status {
        Unknown,
        Old,
        New,
        Moved,
        Deleted,
    };

    enum class PrintObjectRegionsStatus {
        Invalid,
        Valid,
        PartiallyValid,
    };

    ModelObjectStatus(ObjectID id, Status status = Unknown) : id(id), status(status) {}
    ~ModelObjectStatus() { if (print_object_regions) print_object_regions->ref_cnt_dec(); }

    // Key of the set.
    ObjectID                                    id;
    // Status of this ModelObject with id on apply().
    Status                                      status;
    // PrintObjects to be generated for this ModelObject including their base transformation.
    std::vector<PrintObjectTrafoAndInstances>   print_instances;
    // Regions shared by the associated PrintObjects.
    PrintObjectRegions                         *print_object_regions { nullptr };
    // Status of the above.
    PrintObjectRegionsStatus                    print_object_regions_status { PrintObjectRegionsStatus::Invalid };

    // Search by id.
    bool operator<(const ModelObjectStatus &rhs) const { return id < rhs.id; }
};

struct ModelObjectStatusDB
{
    void add(const ModelObject &model_object, const ModelObjectStatus::Status status) {
        assert(db.find(ModelObjectStatus(model_object.id())) == db.end());
        db.emplace(model_object.id(), status);
    }

    bool add_if_new(const ModelObject &model_object, const ModelObjectStatus::Status status) {
        auto it = db.find(ModelObjectStatus(model_object.id()));
        if (it == db.end()) {
            db.emplace_hint(it, model_object.id(), status);
            return true;
        }
        return false;
    }

    const ModelObjectStatus& get(const ModelObject &model_object) {
        auto it = db.find(ModelObjectStatus(model_object.id()));
        assert(it != db.end());
        return *it;
    }

    const ModelObjectStatus& reuse(const ModelObject &model_object) {
        const ModelObjectStatus &result = this->get(model_object);
        assert(result.status != ModelObjectStatus::Deleted);
        return result;
    }

    std::set<ModelObjectStatus> db;
};

struct PrintObjectStatus {
    enum Status {
        Unknown,
        Deleted,
        Reused,
        New
    };

    PrintObjectStatus(PrintObject *print_object, Status status = Unknown) :
        id(print_object->model_object()->id()),
        print_object(print_object),
        trafo(print_object->trafo()),
        status(status) {}
    PrintObjectStatus(ObjectID id) : id(id), print_object(nullptr), trafo(Transform3d::Identity()), status(Unknown) {}

    // ID of the ModelObject & PrintObject
    ObjectID         id;
    // Pointer to the old PrintObject
    PrintObject     *print_object;
    // Trafo generated with model_object->world_matrix(true)
    Transform3d      trafo;
    Status           status;

    // Search by id.
    bool operator<(const PrintObjectStatus &rhs) const { return id < rhs.id; }
};

class PrintObjectStatusDB {
public:
    using iterator          = std::multiset<PrintObjectStatus>::iterator;
    using const_iterator    = std::multiset<PrintObjectStatus>::const_iterator;

    PrintObjectStatusDB(const PrintObjectPtrs &print_objects) {
        for (PrintObject *print_object : print_objects)
            m_db.emplace(PrintObjectStatus(print_object));
    }

    struct iterator_range : std::pair<const_iterator, const_iterator>
    {
        using std::pair<const_iterator, const_iterator>::pair;
        iterator_range(const std::pair<const_iterator, const_iterator> in) : std::pair<const_iterator, const_iterator>(in) {}

        const_iterator begin() throw() { return this->first; }
        const_iterator end() throw() { return this->second; }
    };

    iterator_range get_range(const ModelObject &model_object) const {
        return m_db.equal_range(PrintObjectStatus(model_object.id()));
    }

    iterator_range get_range(const ModelObjectStatus &model_object_status) const {
        return m_db.equal_range(PrintObjectStatus(model_object_status.id));
    }

    size_t count(const ModelObject &model_object) {
        return m_db.count(PrintObjectStatus(model_object.id()));
    }

    std::multiset<PrintObjectStatus>::iterator begin() { return m_db.begin(); }
    std::multiset<PrintObjectStatus>::iterator end()   { return m_db.end(); }

    void clear() {
        m_db.clear();
    }

private:
    std::multiset<PrintObjectStatus> m_db;
};

static inline bool model_volume_solid_or_modifier(const ModelVolume &mv)
{
    ModelVolumeType type = mv.type();
    return type == ModelVolumeType::MODEL_PART || type == ModelVolumeType::NEGATIVE_VOLUME || type == ModelVolumeType::PARAMETER_MODIFIER;
}

static inline Transform3f trafo_for_bbox(const Transform3d &object_trafo, const Transform3d &volume_trafo)
{
    Transform3d m = object_trafo * volume_trafo;
    m.translation().x() = 0.;
    m.translation().y() = 0.;
    return m.cast<float>();
}

static inline bool trafos_differ_in_rotation_by_z_and_mirroring_by_xy_only(const Transform3d &t1, const Transform3d &t2)
{
    if (std::abs(t1.translation().z() - t2.translation().z()) > EPSILON)
        // One of the object is higher than the other above the build plate (or below the build plate).
        return false;
    Matrix3d m1 = t1.matrix().block<3, 3>(0, 0);
    Matrix3d m2 = t2.matrix().block<3, 3>(0, 0);
    Matrix3d m = m2.inverse() * m1;
    Vec3d    z = m.block<3, 1>(0, 2);
    if (std::abs(z.x()) > EPSILON || std::abs(z.y()) > EPSILON || std::abs(z.z() - 1.) > EPSILON)
        // Z direction or length changed.
        return false;
    // Z still points in the same direction and it has the same length.
    Vec3d    x = m.block<3, 1>(0, 0);
    Vec3d    y = m.block<3, 1>(0, 1);
    if (std::abs(x.z()) > EPSILON || std::abs(y.z()) > EPSILON)
        return false;
    double   lx2 = x.squaredNorm();
    double   ly2 = y.squaredNorm();
    if (lx2 - 1. > EPSILON * EPSILON || ly2 - 1. > EPSILON * EPSILON)
        return false;
    // Verify whether the vectors x, y are still perpendicular.
    double   d   = x.dot(y);
    return std::abs(d * d) < EPSILON * lx2 * ly2;
}

static PrintObjectRegions::BoundingBox transformed_its_bbox2d(const indexed_triangle_set &its, const Transform3f &m, float offset)
{
    assert(! its.indices.empty());

    PrintObjectRegions::BoundingBox bbox(m * its.vertices[its.indices.front()(0)]);
    for (const stl_triangle_vertex_indices &tri : its.indices)
        for (int i = 0; i < 3; ++ i)
            bbox.extend(m * its.vertices[tri(i)]);
    bbox.min() -= Vec3f(offset, offset, float(EPSILON));
    bbox.max() += Vec3f(offset, offset, float(EPSILON));
    return bbox;
}

static void transformed_its_bboxes_in_z_ranges(
    const indexed_triangle_set                                    &its,
    const Transform3f                                             &m,
    const std::vector<t_layer_height_range>                       &z_ranges,
    std::vector<std::pair<PrintObjectRegions::BoundingBox, bool>> &bboxes,
    const float                                                    offset)
{
    bboxes.assign(z_ranges.size(), std::make_pair(PrintObjectRegions::BoundingBox(), false));
    for (const stl_triangle_vertex_indices &tri : its.indices) {
        const Vec3f pts[3] = { m * its.vertices[tri(0)], m * its.vertices[tri(1)], m * its.vertices[tri(2)] };
        for (size_t irange = 0; irange < z_ranges.size(); ++ irange) {
            const t_layer_height_range                       &z_range = z_ranges[irange];
            std::pair<PrintObjectRegions::BoundingBox, bool> &bbox    = bboxes[irange];
            auto bbox_extend = [&bbox](const Vec3f& p) {
                if (bbox.second) {
                    bbox.first.extend(p);
                } else {
                    bbox.first.min() = bbox.first.max() = p;
                    bbox.second = true;
                }
            };
            int iprev = 2;
            for (int iedge = 0; iedge < 3; ++ iedge) {
                const Vec3f *p1 = &pts[iprev];
                const Vec3f *p2 = &pts[iedge];
                // Sort the edge points by Z.
                if (p1->z() > p2->z())
                    std::swap(p1, p2);
                if (p2->z() <= z_range.first || p1->z() >= z_range.second) {
                    // Out of this slab.
                } else if (p1->z() < z_range.first) {
                    if (p2->z() > z_range.second) {
                        // Two intersections.
                        float zspan = p2->z() - p1->z();
                        float t1 = (z_range.first - p1->z())  / zspan;
                        float t2 = (z_range.second - p1->z()) / zspan;
                        Vec2f p = to_2d(*p1);
                        Vec2f v(p2->x() - p1->x(), p2->y() - p1->y());
                        bbox_extend(to_3d((p + v * t1).eval(), float(z_range.first)));
                        bbox_extend(to_3d((p + v * t2).eval(), float(z_range.second)));
                    } else {
                        // Single intersection with the lower limit.
                        float t = (z_range.first - p1->z()) / (p2->z() - p1->z());
                        Vec2f v(p2->x() - p1->x(), p2->y() - p1->y());
                        bbox_extend(to_3d((to_2d(*p1) + v * t).eval(), float(z_range.first)));
                        bbox_extend(*p2);
                    }
                } else if (p2->z() > z_range.second) {
                    // Single intersection with the upper limit.
                    float t = (z_range.second - p1->z()) / (p2->z() - p1->z());
                    Vec2f v(p2->x() - p1->x(), p2->y() - p1->y());
                    bbox_extend(to_3d((to_2d(*p1) + v * t).eval(), float(z_range.second)));
                    bbox_extend(*p1);
                } else {
                    // Both points are inside.
                    bbox_extend(*p1);
                    bbox_extend(*p2);
                }
                iprev = iedge;
            }
        }
    }

    for (std::pair<PrintObjectRegions::BoundingBox, bool> &bbox : bboxes) {
        bbox.first.min() -= Vec3f(offset, offset, float(EPSILON));
        bbox.first.max() += Vec3f(offset, offset, float(EPSILON));
    }
}

// Last PrintObject for this print_object_regions has been fully invalidated (deleted).
// Keep print_object_regions, but delete those volumes, which were either removed from new_volumes, or which rotated or scaled, so they need
// their bounding boxes to be recalculated.
void print_objects_regions_invalidate_keep_some_volumes(PrintObjectRegions &print_object_regions, ModelVolumePtrs old_volumes, ModelVolumePtrs new_volumes)
{
    print_object_regions.all_regions.clear();

    model_volumes_sort_by_id(old_volumes);
    model_volumes_sort_by_id(new_volumes);

    size_t i_cached_volume      = 0;
    size_t last_cached_volume   = 0;
    size_t i_old                = 0;
    for (size_t i_new = 0; i_new < new_volumes.size(); ++ i_new)
        if (model_volume_solid_or_modifier(*new_volumes[i_new])) {
            for (; i_old < old_volumes.size(); ++ i_old)
                if (old_volumes[i_old]->id() >= new_volumes[i_new]->id())
                    break;
            if (i_old != old_volumes.size() && old_volumes[i_old]->id() == new_volumes[i_new]->id()) {
                if (old_volumes[i_old]->get_matrix().isApprox(new_volumes[i_new]->get_matrix())) {
                    // Reuse the volume.
                    for (; print_object_regions.cached_volume_ids[i_cached_volume] < old_volumes[i_old]->id(); ++ i_cached_volume)
                        assert(i_cached_volume < print_object_regions.cached_volume_ids.size());
                    assert(i_cached_volume < print_object_regions.cached_volume_ids.size() && print_object_regions.cached_volume_ids[i_cached_volume] == old_volumes[i_old]->id());
                    print_object_regions.cached_volume_ids[last_cached_volume ++] = print_object_regions.cached_volume_ids[i_cached_volume ++];
                } else {
                    // Don't reuse the volume.
                }
            }
        }
    print_object_regions.cached_volume_ids.erase(print_object_regions.cached_volume_ids.begin() + last_cached_volume, print_object_regions.cached_volume_ids.end());
}

// Find a bounding box of a volume's part intersecting layer_range. Such a bounding box will likely be smaller in XY than the full bounding box,
// thus it will intersect with lower number of other volumes.
const PrintObjectRegions::BoundingBox* find_volume_extents(const PrintObjectRegions::LayerRangeRegions &layer_range, const ModelVolume &volume)
{
    auto it = lower_bound_by_predicate(layer_range.volumes.begin(), layer_range.volumes.end(), [&volume](const PrintObjectRegions::VolumeExtents &l){ return l.volume_id < volume.id(); });
    return it != layer_range.volumes.end() && it->volume_id == volume.id() ? &it->bbox : nullptr;
}

// Find a bounding box of a topmost printable volume referenced by this modifier given this_region_id.
PrintObjectRegions::BoundingBox find_modifier_volume_extents(const PrintObjectRegions::LayerRangeRegions &layer_range, const int this_region_id)
{
    // Find the top-most printable volume of this modifier, or the printable volume itself.
    const PrintObjectRegions::VolumeRegion &this_region = layer_range.volume_regions[this_region_id];
    const PrintObjectRegions::BoundingBox *this_extents = find_volume_extents(layer_range, *this_region.model_volume);
    assert(this_extents);
    PrintObjectRegions::BoundingBox out { *this_extents };
    if (! this_region.model_volume->is_model_part())
        for (int parent_region_id = this_region.parent;;) {
            assert(parent_region_id >= 0);
            const PrintObjectRegions::VolumeRegion &parent_region  = layer_range.volume_regions[parent_region_id];
            const PrintObjectRegions::BoundingBox  *parent_extents = find_volume_extents(layer_range, *parent_region.model_volume);
            assert(parent_extents);
            out.extend(*parent_extents);
            if (parent_region.model_volume->is_model_part())
                break;
            parent_region_id = parent_region.parent;
        }
    return out;
}

PrintRegionConfig region_config_from_model_volume(const PrintRegionConfig &default_or_parent_region_config, const DynamicPrintConfig *layer_range_config, const ModelVolume &volume, size_t num_extruders);

void print_region_ref_inc(PrintRegion &r) { ++ r.m_ref_cnt; }
void print_region_ref_reset(PrintRegion &r) { r.m_ref_cnt = 0; }
int  print_region_ref_cnt(const PrintRegion &r) { return r.m_ref_cnt; }

// Verify whether the PrintRegions of a PrintObject are still valid, possibly after updating the region configs.
// Before region configs are updated, callback_invalidate() is called to possibly stop background processing.
// Returns false if this object needs to be resliced because regions were merged or split.
bool verify_update_print_object_regions(
    ModelVolumePtrs                     model_volumes,
    const PrintRegionConfig            &default_region_config,
    size_t                              num_extruders,
    PrintObjectRegions                 &print_object_regions,
    const std::function<void(const PrintRegionConfig&, const PrintRegionConfig&, const t_config_option_keys&)> &callback_invalidate)
{
    // Sort by ModelVolume ID.
    model_volumes_sort_by_id(model_volumes);

    for (std::unique_ptr<PrintRegion> &region : print_object_regions.all_regions)
        print_region_ref_reset(*region);

    // Verify and / or update PrintRegions produced by ModelVolumes, layer range modifiers, modifier volumes.
    for (PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
        // Each modifier ModelVolume intersecting this layer_range shall be referenced here at least once if it intersects some
        // printable ModelVolume at this layer_range even if it does not modify its overlapping printable ModelVolume configuration yet.
        // VolumeRegions reference ModelVolumes in layer_range.volume_regions the order they are stored in ModelObject volumes.
        // Remember whether a given modifier ModelVolume was visited already.
        auto it_model_volume_modifier_last = model_volumes.end();
        for (PrintObjectRegions::VolumeRegion &region : layer_range.volume_regions)
            if (region.model_volume->is_model_part() || region.model_volume->is_modifier()) {
                auto it_model_volume = lower_bound_by_predicate(model_volumes.begin(), model_volumes.end(), [&region](const ModelVolume *l){ return l->id() < region.model_volume->id(); });
                assert(it_model_volume != model_volumes.end() && (*it_model_volume)->id() == region.model_volume->id());
                if (region.model_volume->is_modifier() && it_model_volume != it_model_volume_modifier_last) {
                    // A modifier ModelVolume is visited for the first time.
                    // A visited modifier may not have had parent volume_regions created overlapping with some model parts or modifiers,
                    // if the visited modifier did not modify their properties. Now the visited modifier's configuration may have changed,
                    // which may require new regions to be created.
                    it_model_volume_modifier_last = it_model_volume;
                    int next_region_id = int(&region - layer_range.volume_regions.data());
                    const PrintObjectRegions::BoundingBox *bbox = find_volume_extents(layer_range, *region.model_volume);
                    assert(bbox);
                    for (int parent_region_id = next_region_id - 1; parent_region_id >= 0; -- parent_region_id) {
                        const PrintObjectRegions::VolumeRegion &parent_region = layer_range.volume_regions[parent_region_id];
                        assert(parent_region.model_volume != region.model_volume);
                        if (parent_region.model_volume->is_model_part() || parent_region.model_volume->is_modifier()) {
                            // volume_regions are produced in decreasing order of parent volume_regions ids.
                            // Some regions may not have been generated the last time by generate_print_object_regions().
                            assert(next_region_id == int(layer_range.volume_regions.size()) ||
                                   layer_range.volume_regions[next_region_id].model_volume != region.model_volume ||
                                   layer_range.volume_regions[next_region_id].parent <= parent_region_id);
                            if (next_region_id < int(layer_range.volume_regions.size()) &&
                                layer_range.volume_regions[next_region_id].model_volume == region.model_volume &&
                                layer_range.volume_regions[next_region_id].parent == parent_region_id) {
                                // A parent region is already overridden.
                                ++ next_region_id;
                            } else if (PrintObjectRegions::BoundingBox parent_bbox = find_modifier_volume_extents(layer_range, parent_region_id); parent_bbox.intersects(*bbox))
                                // Such parent region does not exist. If it is needed, then we need to reslice.
                                // Only create new region for a modifier, which actually modifies config of it's parent.
                                if (PrintRegionConfig config = region_config_from_model_volume(parent_region.region->config(), nullptr, **it_model_volume, num_extruders);
                                    config != parent_region.region->config())
                                    // This modifier newly overrides a region, which it did not before. We need to reslice.
                                    return false;
                        }
                    }
                }
                PrintRegionConfig cfg = region.parent == -1 ?
                    region_config_from_model_volume(default_region_config, layer_range.config, **it_model_volume, num_extruders) :
                    region_config_from_model_volume(layer_range.volume_regions[region.parent].region->config(), nullptr, **it_model_volume, num_extruders);
                if (cfg != region.region->config()) {
                    // Region configuration changed.
                    if (print_region_ref_cnt(*region.region) == 0) {
                        // Region is referenced for the first time. Just change its parameters.
                        // Stop the background process before assigning new configuration to the regions.
                        t_config_option_keys diff = region.region->config().diff(cfg);
                        callback_invalidate(region.region->config(), cfg, diff);
                        region.region->config_apply_only(cfg, diff, false);
                    } else {
                        // Region is referenced multiple times, thus the region is being split. We need to reslice.
                        return false;
                    }
                }
                print_region_ref_inc(*region.region);
            }
    }

    // Verify and / or update PrintRegions produced by color painting.
    for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges)
        for (const PrintObjectRegions::PaintedRegion &region : layer_range.painted_regions) {
            const PrintObjectRegions::VolumeRegion &parent_region   = layer_range.volume_regions[region.parent];
            PrintRegionConfig                       cfg             = parent_region.region->config();
            cfg.wall_filament.value    = region.extruder_id;
            cfg.solid_infill_filament.value = region.extruder_id;
            cfg.sparse_infill_filament.value       = region.extruder_id;
            if (cfg != region.region->config()) {
                // Region configuration changed.
                if (print_region_ref_cnt(*region.region) == 0) {
                    // Region is referenced for the first time. Just change its parameters.
                    // Stop the background process before assigning new configuration to the regions.
                    t_config_option_keys diff = region.region->config().diff(cfg);
                    callback_invalidate(region.region->config(), cfg, diff);
                    region.region->config_apply_only(cfg, diff, false);
                } else {
                    // Region is referenced multiple times, thus the region is being split. We need to reslice.
                    return false;
                }
            }
            print_region_ref_inc(*region.region);
        }

    // Verify and / or update PrintRegions produced by fuzzy skin painting.
    for (const PrintObjectRegions::LayerRangeRegions &layer_range : print_object_regions.layer_ranges) {
        for (const PrintObjectRegions::FuzzySkinPaintedRegion &region : layer_range.fuzzy_skin_painted_regions) {
            const PrintRegion &parent_print_region = *region.parent_print_object_region(layer_range);
            PrintRegionConfig  cfg                 = parent_print_region.config();
            if (cfg.fuzzy_skin.value != FuzzySkinType::Disabled_fuzzy) cfg.fuzzy_skin.value = FuzzySkinType::All;
            if (cfg != region.region->config()) {
                // Region configuration changed.
                if (print_region_ref_cnt(*region.region) == 0) {
                    // Region is referenced for the first time. Just change its parameters.
                    // Stop the background process before assigning new configuration to the regions.
                    t_config_option_keys diff = region.region->config().diff(cfg);
                    callback_invalidate(region.region->config(), cfg, diff);
                    region.region->config_apply_only(cfg, diff, false);
                } else {
                    // Region is referenced multiple times, thus the region is being split. We need to reslice.
                    return false;
                }
            }
            print_region_ref_inc(*region.region);
        }
    }

    // Lastly verify, whether some regions were not merged.
    {
        std::vector<const PrintRegion*> regions;
        regions.reserve(print_object_regions.all_regions.size());
        for (std::unique_ptr<PrintRegion> &region : print_object_regions.all_regions) {
            assert(print_region_ref_cnt(*region) > 0);
            regions.emplace_back(&(*region.get()));
        }
        std::sort(regions.begin(), regions.end(), [](const PrintRegion *l, const PrintRegion *r){ return l->config_hash() < r->config_hash(); });
        for (size_t i = 0; i < regions.size(); ++ i) {
            size_t hash = regions[i]->config_hash();
            size_t j = i;
            for (++ j; j < regions.size() && regions[j]->config_hash() == hash; ++ j)
                if (regions[i]->config() == regions[j]->config()) {
                    // Regions were merged. We need to reslice.
                    return false;
                }
        }
    }

    return true;
}

// Update caches of volume bounding boxes.
void update_volume_bboxes(
    std::vector<PrintObjectRegions::LayerRangeRegions>  &layer_ranges,
    std::vector<ObjectID>                               &cached_volume_ids,
    ModelVolumePtrs                                      model_volumes,
    const Transform3d                                   &object_trafo,
    const float                                          offset)
{
    // output will be sorted by the order of model_volumes sorted by their ObjectIDs.
    model_volumes_sort_by_id(model_volumes);

    if (layer_ranges.size() == 1) {
        PrintObjectRegions::LayerRangeRegions &layer_range = layer_ranges.front();
        std::vector<PrintObjectRegions::VolumeExtents> volumes_old(std::move(layer_range.volumes));
        layer_range.volumes.reserve(model_volumes.size());
        for (const ModelVolume *model_volume : model_volumes)
            if (model_volume_solid_or_modifier(*model_volume)) {
                if (std::binary_search(cached_volume_ids.begin(), cached_volume_ids.end(), model_volume->id())) {
                    auto it = lower_bound_by_predicate(volumes_old.begin(), volumes_old.end(), [model_volume](PrintObjectRegions::VolumeExtents &l) { return l.volume_id < model_volume->id(); });
                    if (it != volumes_old.end() && it->volume_id == model_volume->id())
                        layer_range.volumes.emplace_back(*it);
                } else
                    layer_range.volumes.push_back({ model_volume->id(),
                        transformed_its_bbox2d(model_volume->mesh().its, trafo_for_bbox(object_trafo, model_volume->get_matrix()), offset) });
            }
    } else {
        std::vector<std::vector<PrintObjectRegions::VolumeExtents>> volumes_old;
        if (cached_volume_ids.empty())
            for (PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges)
                layer_range.volumes.clear();
        else {
            volumes_old.reserve(layer_ranges.size());
            for (PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges)
                volumes_old.emplace_back(std::move(layer_range.volumes));
        }

        std::vector<std::pair<PrintObjectRegions::BoundingBox, bool>> bboxes;
        std::vector<t_layer_height_range>                             ranges;
        ranges.reserve(layer_ranges.size());
        for (const PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges) {
            t_layer_height_range r = layer_range.layer_height_range;
            r.first  -= EPSILON;
            r.second += EPSILON;
            ranges.emplace_back(r);
        }
        for (const ModelVolume *model_volume : model_volumes)
            if (model_volume_solid_or_modifier(*model_volume)) {
                if (std::binary_search(cached_volume_ids.begin(), cached_volume_ids.end(), model_volume->id())) {
                    for (PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges) {
                        const auto &vold = volumes_old[&layer_range - layer_ranges.data()];
                        auto it = lower_bound_by_predicate(vold.begin(), vold.end(), [model_volume](const PrintObjectRegions::VolumeExtents &l) { return l.volume_id < model_volume->id(); });
                        if (it != vold.end() && it->volume_id == model_volume->id())
                            layer_range.volumes.emplace_back(*it);
                    }
                } else {
                    transformed_its_bboxes_in_z_ranges(model_volume->mesh().its, trafo_for_bbox(object_trafo, model_volume->get_matrix()), ranges, bboxes, offset);
                    for (PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges)
                        if (auto &bbox = bboxes[&layer_range - layer_ranges.data()]; bbox.second)
                            layer_range.volumes.push_back({ model_volume->id(), bbox.first });
                }
            }
    }

    cached_volume_ids.clear();
    cached_volume_ids.reserve(model_volumes.size());
    for (const ModelVolume *v : model_volumes)
        if (model_volume_solid_or_modifier(*v))
            cached_volume_ids.emplace_back(v->id());
}

// Either a fresh PrintObject, or PrintObject regions were invalidated (merged, split).
// Generate PrintRegions from scratch.
static PrintObjectRegions* generate_print_object_regions(
    PrintObjectRegions                          *print_object_regions_old,
    const ModelVolumePtrs                       &model_volumes,
    const LayerRanges                           &model_layer_ranges,
    const PrintRegionConfig                     &default_region_config,
    const Transform3d                           &trafo,
    size_t                                       num_extruders,
    const float                                  xy_contour_compensation,
    const std::vector<unsigned int>             &painting_extruders,
    const bool                                   has_painted_fuzzy_skin)
{
    // Reuse the old object or generate a new one.
    auto out = print_object_regions_old ? std::unique_ptr<PrintObjectRegions>(print_object_regions_old) : std::make_unique<PrintObjectRegions>();
    auto &all_regions          = out->all_regions;
    auto &layer_ranges_regions = out->layer_ranges;

    all_regions.clear();

    bool reuse_old = print_object_regions_old && !print_object_regions_old->layer_ranges.empty();

    if (reuse_old) {
        // Reuse old bounding boxes of some ModelVolumes and their ranges.
        // Verify that the old ranges match the new ranges.
        assert(model_layer_ranges.size() == layer_ranges_regions.size());
        for (const auto &range : model_layer_ranges) {
            PrintObjectRegions::LayerRangeRegions &r = layer_ranges_regions[&range - &*model_layer_ranges.begin()];
            assert(range.layer_height_range == r.layer_height_range);
            // If model::assign_copy() is called, layer_ranges_regions is copied thus the pointers to configs are lost.
            r.config = range.config;
            r.volume_regions.clear();
            r.painted_regions.clear();
            r.fuzzy_skin_painted_regions.clear();
        }
    } else {
        out->trafo_bboxes = trafo;
        layer_ranges_regions.reserve(model_layer_ranges.size());
        for (const auto &range : model_layer_ranges)
            layer_ranges_regions.push_back({ range.layer_height_range, range.config });
    }

    const bool is_mm_painted = num_extruders > 1 && std::any_of(model_volumes.cbegin(), model_volumes.cend(), [](const ModelVolume *mv) { return mv->is_mm_painted(); });
    update_volume_bboxes(layer_ranges_regions, out->cached_volume_ids, model_volumes, out->trafo_bboxes, is_mm_painted ? 0.f : std::max(0.f, xy_contour_compensation));

    std::vector<PrintRegion*> region_set;
    auto get_create_region = [&region_set, &all_regions](PrintRegionConfig &&config) -> PrintRegion* {
        size_t hash = config.hash();
        auto it = Slic3r::lower_bound_by_predicate(region_set.begin(), region_set.end(), [&config, hash](const PrintRegion* l) {
            return l->config_hash() < hash || (l->config_hash() == hash && l->config() < config); });
        if (it != region_set.end() && (*it)->config_hash() == hash && (*it)->config() == config)
            return *it;
        // Insert into a sorted array, it has O(n) complexity, but the calling algorithm has an O(n^2*log(n)) complexity anyways.
        all_regions.emplace_back(std::make_unique<PrintRegion>(std::move(config), hash, int(all_regions.size())));
        PrintRegion *region = all_regions.back().get();
        region_set.emplace(it, region);
        return region;
    };

    // Chain the regions in the order they are stored in the volumes list.
    for (int volume_id = 0; volume_id < int(model_volumes.size()); ++ volume_id) {
        const ModelVolume &volume = *model_volumes[volume_id];
        if (model_volume_solid_or_modifier(volume)) {
            for (PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges_regions)
                if (const PrintObjectRegions::BoundingBox *bbox = find_volume_extents(layer_range, volume); bbox) {
                    if (volume.is_model_part()) {
                        // Add a model volume, assign an existing region or generate a new one.
                        layer_range.volume_regions.push_back({
                            &volume, -1,
                            get_create_region(region_config_from_model_volume(default_region_config, layer_range.config, volume, num_extruders)),
                            bbox
                        });
                    } else if (volume.is_negative_volume()) {
                        // Add a negative (subtractor) volume. Such volume has neither region nor parent volume assigned.
                        layer_range.volume_regions.push_back({ &volume, -1, nullptr, bbox });
                    } else {
                        assert(volume.is_modifier());
                        // Modifiers may be chained one over the other. Check for overlap, merge DynamicPrintConfigs.
                        bool added = false;
                        int  parent_model_part_id = -1;
                        for (int parent_region_id = int(layer_range.volume_regions.size()) - 1; parent_region_id >= 0; -- parent_region_id) {
                            const PrintObjectRegions::VolumeRegion &parent_region = layer_range.volume_regions[parent_region_id];
                            const ModelVolume                      &parent_volume = *parent_region.model_volume;
                            if (parent_volume.is_model_part() || parent_volume.is_modifier())
                                if (PrintObjectRegions::BoundingBox parent_bbox = find_modifier_volume_extents(layer_range, parent_region_id); parent_bbox.intersects(*bbox)) {
                                    // Only create new region for a modifier, which actually modifies config of it's parent.
                                    if (PrintRegionConfig config = region_config_from_model_volume(parent_region.region->config(), nullptr, volume, num_extruders);
                                        config != parent_region.region->config()) {
                                        added = true;
                                        layer_range.volume_regions.push_back({ &volume, parent_region_id, get_create_region(std::move(config)), bbox });
                                    } else if (parent_model_part_id == -1 && parent_volume.is_model_part())
                                        parent_model_part_id = parent_region_id;
                                }
                        }
                        if (! added && parent_model_part_id >= 0)
                            // This modifier does not override any printable volume's configuration, however it may in the future.
                            // Store it so that verify_update_print_object_regions() will handle this modifier correctly if its configuration changes.
                            layer_range.volume_regions.push_back({ &volume, parent_model_part_id, layer_range.volume_regions[parent_model_part_id].region, bbox });
                    }
                }
            }
    }

    // Finally add painting regions.
    for (PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges_regions) {
        for (unsigned int painted_extruder_id : painting_extruders)
            for (int parent_region_id = 0; parent_region_id < int(layer_range.volume_regions.size()); ++ parent_region_id)
                if (const PrintObjectRegions::VolumeRegion &parent_region = layer_range.volume_regions[parent_region_id];
                    parent_region.model_volume->is_model_part() || parent_region.model_volume->is_modifier()) {
                    PrintRegionConfig cfg = parent_region.region->config();
                    cfg.wall_filament.value    = painted_extruder_id;
                    cfg.solid_infill_filament.value = painted_extruder_id;
                    cfg.sparse_infill_filament.value       = painted_extruder_id;
                    layer_range.painted_regions.push_back({ painted_extruder_id, parent_region_id, get_create_region(std::move(cfg))});
                }
        // Sort the regions by parent region::print_object_region_id() and extruder_id to help the slicing algorithm when applying MM segmentation.
        std::sort(layer_range.painted_regions.begin(), layer_range.painted_regions.end(), [&layer_range](auto &l, auto &r) {
            int lid = layer_range.volume_regions[l.parent].region->print_object_region_id();
            int rid = layer_range.volume_regions[r.parent].region->print_object_region_id();
            return lid < rid || (lid == rid && l.extruder_id < r.extruder_id); });
    }

    if (has_painted_fuzzy_skin) {
        using FuzzySkinParentType = PrintObjectRegions::FuzzySkinPaintedRegion::ParentType;

        for (PrintObjectRegions::LayerRangeRegions &layer_range : layer_ranges_regions) {
            // FuzzySkinPaintedRegion can override different parts of the Layer than PaintedRegions,
            // so FuzzySkinPaintedRegion has to point to both VolumeRegion and PaintedRegion.
            for (int parent_volume_region_id = 0; parent_volume_region_id < int(layer_range.volume_regions.size()); ++parent_volume_region_id) {
                if (const PrintObjectRegions::VolumeRegion &parent_volume_region = layer_range.volume_regions[parent_volume_region_id]; parent_volume_region.model_volume->is_model_part() || parent_volume_region.model_volume->is_modifier()) {
                    PrintRegionConfig cfg = parent_volume_region.region->config();
                    if (cfg.fuzzy_skin.value != FuzzySkinType::Disabled_fuzzy) cfg.fuzzy_skin.value = FuzzySkinType::All;
                    layer_range.fuzzy_skin_painted_regions.push_back({FuzzySkinParentType::VolumeRegion, parent_volume_region_id, get_create_region(std::move(cfg))});
                }
            }

            for (int parent_painted_regions_id = 0; parent_painted_regions_id < int(layer_range.painted_regions.size()); ++parent_painted_regions_id) {
                const PrintObjectRegions::PaintedRegion &parent_painted_region = layer_range.painted_regions[parent_painted_regions_id];

                PrintRegionConfig cfg = parent_painted_region.region->config();
                if (cfg.fuzzy_skin.value != FuzzySkinType::Disabled_fuzzy) cfg.fuzzy_skin.value = FuzzySkinType::All;
                layer_range.fuzzy_skin_painted_regions.push_back({FuzzySkinParentType::PaintedRegion, parent_painted_regions_id, get_create_region(std::move(cfg))});
            }

            // Sort the regions by parent region::print_object_region_id() to help the slicing algorithm when applying fuzzy skin segmentation.
            std::sort(layer_range.fuzzy_skin_painted_regions.begin(), layer_range.fuzzy_skin_painted_regions.end(), [&layer_range](auto &l, auto &r) {
                return l.parent_print_object_region_id(layer_range) < r.parent_print_object_region_id(layer_range);
            });
        }
    }

    return out.release();
}

Print::ApplyStatus Print::apply(const Model &model, DynamicPrintConfig new_full_config)
{
#ifdef _DEBUG
    check_model_ids_validity(model);
#endif /* _DEBUG */

    //BBS: add more logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: enter")%__LINE__;
    // Normalize the config.
	new_full_config.option("print_settings_id",            true);
	new_full_config.option("filament_settings_id",         true);
	new_full_config.option("printer_settings_id",          true);

    // BBS
    std::vector <unsigned int> used_filaments = this->extruders(true);
    std::unordered_set <unsigned int> used_filament_set(used_filaments.begin(), used_filaments.end());

    //new_full_config.normalize_fdm(used_filaments);
    new_full_config.normalize_fdm_1();
    t_config_option_keys changed_keys = new_full_config.normalize_fdm_2(objects().size(), used_filaments.size());
    if (changed_keys.size() > 0) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", got changed_keys, size=%1%")%changed_keys.size();
        for (int i = 0; i < changed_keys.size(); i++)
        {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", i=%1%, key=%2%")%i %changed_keys[i];
        }
    }
    const ConfigOption* enable_support_option = new_full_config.option("enable_support");
    if (enable_support_option && enable_support_option->getBool())
        m_support_used = true;
    else
        m_support_used = false;

    {
        const auto& o = model.objects;
        const auto opt_has_scarf_joint_seam = [](const DynamicConfig& c) {
            return c.has("seam_slope_type") && c.opt_enum<SeamScarfType>("seam_slope_type") != SeamScarfType::None;
        };
        const bool has_scarf_joint_seam = std::any_of(o.begin(), o.end(), [&new_full_config, &opt_has_scarf_joint_seam](ModelObject* obj) {
            return obj->get_config_value<ConfigOptionEnum<SeamScarfType>>(new_full_config, "seam_slope_type")->value != SeamScarfType::None ||
                   std::any_of(obj->volumes.begin(), obj->volumes.end(), [&opt_has_scarf_joint_seam](const ModelVolume* v) { return opt_has_scarf_joint_seam(v->config.get());}) ||
                   std::any_of(obj->layer_config_ranges.begin(), obj->layer_config_ranges.end(), [&opt_has_scarf_joint_seam](const auto& r) { return opt_has_scarf_joint_seam(r.second.get());});
        });

        if (has_scarf_joint_seam) {
            new_full_config.set("has_scarf_joint_seam", true);
        }

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", has_scarf_joint_seam:" << has_scarf_joint_seam;
    }

    //apply extruder related values
    new_full_config.update_values_to_printer_extruders(new_full_config, printer_options_with_variant_1, "printer_extruder_id", "printer_extruder_variant");
    new_full_config.update_values_to_printer_extruders(new_full_config, printer_options_with_variant_2, "printer_extruder_id", "printer_extruder_variant", 2);
    //update print config related with variants
    new_full_config.update_values_to_printer_extruders(new_full_config, print_options_with_variant, "print_extruder_id", "print_extruder_variant");

    m_ori_full_print_config = new_full_config;
    new_full_config.update_values_to_printer_extruders_for_multiple_filaments(new_full_config, filament_options_with_variant,  "filament_self_index", "filament_extruder_variant");
    std::vector<int> filament_maps =  new_full_config.option<ConfigOptionInts>("filament_map")->values;

    // Find modified keys of the various configs. Resolve overrides extruder retract values by filament profiles.
    DynamicPrintConfig   filament_overrides;
    //BBS: add plate index
    t_config_option_keys print_diff       = print_config_diffs(m_config, new_full_config, filament_overrides, this->m_plate_index, filament_maps);
    t_config_option_keys full_config_diff = full_print_config_diffs(m_full_print_config, new_full_config, this->m_plate_index);
    // Collect changes to object and region configs.
    t_config_option_keys object_diff      = m_default_object_config.diff(new_full_config);
    t_config_option_keys region_diff      = m_default_region_config.diff(new_full_config);

    //BBS: process the filament_map related logic
    std::unordered_set<std::string> print_diff_set(print_diff.begin(), print_diff.end());
    if (print_diff_set.find("filament_map_mode") == print_diff_set.end())
    {
        FilamentMapMode map_mode = new_full_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value;
        if (map_mode < fmmManual) {
            if (print_diff_set.find("filament_map") != print_diff_set.end()) {
                print_diff_set.erase("filament_map");
                //full_config_diff.erase("filament_map");
                ConfigOptionInts* old_opt = m_full_print_config.option<ConfigOptionInts>("filament_map", true);
                ConfigOptionInts* new_opt = new_full_config.option<ConfigOptionInts>("filament_map", true);
                old_opt->set(new_opt);
                m_config.filament_map = *new_opt;
            }
        }
        else {
            print_diff_set.erase("extruder_ams_count");
            std::vector<int> old_filament_map = m_config.filament_map.values;
            std::vector<int> new_filament_map = new_full_config.option<ConfigOptionInts>("filament_map", true)->values;

            if (old_filament_map.size() == new_filament_map.size())
            {
                bool same_map = true;
                for (size_t index = 0; index < old_filament_map.size(); index++)
                {
                    if ((old_filament_map[index] == new_filament_map[index])
                        || (used_filament_set.find(index) == used_filament_set.end()))
                        continue;
                    else {
                        same_map = false;
                        break;
                    }
                }
                if (same_map)
                    print_diff_set.erase("filament_map");
            }
        }
        if (print_diff_set.size() != print_diff.size())
            print_diff.assign(print_diff_set.begin(), print_diff_set.end());
    }

    // Do not use the ApplyStatus as we will use the max function when updating apply_status.
    unsigned int apply_status = APPLY_STATUS_UNCHANGED;
    auto update_apply_status = [&apply_status](bool invalidated)
        { apply_status = std::max<unsigned int>(apply_status, invalidated ? APPLY_STATUS_INVALIDATED : APPLY_STATUS_CHANGED); };
    if (! (print_diff.empty() && object_diff.empty() && region_diff.empty())) {
        update_apply_status(false);
        //BBS: add more logs
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", got print_diff %1%, object_diff %2%, region_diff %3%, set status to APPLY_STATUS_CHANGED")%print_diff.size() %object_diff.size() %region_diff.size();
    }

    // Grab the lock for the Print / PrintObject milestones.
	std::scoped_lock<std::mutex> lock(this->state_mutex());

    // The following call may stop the background processing.
    if (! print_diff.empty())
        update_apply_status(this->invalidate_state_by_config_options(new_full_config, print_diff));

    // Apply variables to placeholder parser. The placeholder parser is used by G-code export,
    // which should be stopped if print_diff is not empty.
    size_t num_extruders  = m_config.filament_diameter.size();
    bool   num_extruders_changed  = false;
    if (! full_config_diff.empty()) {
        //BBS: add more logs
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: found full_config_diff changed.")%__LINE__;
        update_apply_status(this->invalidate_step(psGCodeExport));
        m_placeholder_parser.clear_config();
        // Set the profile aliases for the PrintBase::output_filename()
		m_placeholder_parser.set("print_preset",              new_full_config.option("print_settings_id")->clone());
		m_placeholder_parser.set("filament_preset",           new_full_config.option("filament_settings_id")->clone());
		m_placeholder_parser.set("printer_preset",            new_full_config.option("printer_settings_id")->clone());
		// We want the filament overrides to be applied over their respective extruder parameters by the PlaceholderParser.
		// see "Placeholders do not respect filament overrides." GH issue #3649
		m_placeholder_parser.apply_config(filament_overrides);
	    // It is also safe to change m_config now after this->invalidate_state_by_config_options() call.
	    m_config.apply_only(new_full_config, print_diff, true);
	    //FIXME use move semantics once ConfigBase supports it.
        // Some filament_overrides may contain values different from new_full_config, but equal to m_config.
        // As long as these config options don't reallocate memory when copying, we are safe overriding a value, which is in use by a worker thread.
	    m_config.apply(filament_overrides);
	    // Handle changes to object config defaults
	    m_default_object_config.apply_only(new_full_config, object_diff, true);
	    // Handle changes to regions config defaults
	    m_default_region_config.apply_only(new_full_config, region_diff, true);
        //m_full_print_config = std::move(new_full_config);
        m_full_print_config = new_full_config;
        if (num_extruders  != m_config.filament_diameter.size()) {
            num_extruders  = m_config.filament_diameter.size();
            num_extruders_changed  = true;
        }
    }

    ModelObjectStatusDB model_object_status_db;

    // 1) Synchronize model objects.
    bool print_regions_reshuffled = false;
    if (model.id() != m_model.id()) {
        // Kill everything, initialize from scratch.
        // Stop background processing.
        this->call_cancel_callback();
        update_apply_status(this->invalidate_all_steps());
        for (PrintObject *object : m_objects) {
            model_object_status_db.add(*object->model_object(), ModelObjectStatus::Deleted);
			update_apply_status(object->invalidate_all_steps());
			delete object;
        }
        m_objects.clear();
        print_regions_reshuffled = true;
        m_model.assign_copy(model);
		for (const ModelObject *model_object : m_model.objects)
			model_object_status_db.add(*model_object, ModelObjectStatus::New);
    } else {
        //BBS: replace model custom gcode with current plate custom gcode
        m_model.curr_plate_index = model.curr_plate_index;
        if (m_model.get_curr_plate_custom_gcodes() != model.get_curr_plate_custom_gcodes()) {
            update_apply_status(num_extruders_changed ||
                // Tool change G-codes are applied as color changes for a single extruder printer, no need to invalidate tool ordering.
                //FIXME The tool ordering may be invalidated unnecessarily if the custom_gcode_per_print_z.mode is not applicable
                // to the active print / model state, and then it is reset, so it is being applicable, but empty, thus the effect is the same.
                (num_extruders > 1 && custom_per_printz_gcodes_tool_changes_differ(m_model.get_curr_plate_custom_gcodes().gcodes, model.get_curr_plate_custom_gcodes().gcodes)) ?
                // The Tool Ordering and the Wipe Tower are no more valid.
                this->invalidate_steps({ psWipeTower, psGCodeExport }) :
                // There is no change in Tool Changes stored in custom_gcode_per_print_z, therefore there is no need to update Tool Ordering.
                this->invalidate_step(psGCodeExport));
            m_model.plates_custom_gcodes[m_model.curr_plate_index] = model.get_curr_plate_custom_gcodes();
        }
        if (model_object_list_equal(m_model, model)) {
            // The object list did not change.
			for (const ModelObject *model_object : m_model.objects)
				model_object_status_db.add(*model_object, ModelObjectStatus::Old);
        } else if (model_object_list_extended(m_model, model)) {
            // Add new objects. Their volumes and configs will be synchronized later.
            //BBS: we don't need to set invalid here, we judge it by comparing the print_object list
            //update_apply_status(this->invalidate_step(psGCodeExport));
            for (const ModelObject *model_object : m_model.objects)
                model_object_status_db.add(*model_object, ModelObjectStatus::Old);
            for (size_t i = m_model.objects.size(); i < model.objects.size(); ++ i) {
                model_object_status_db.add(*model.objects[i], ModelObjectStatus::New);
                m_model.objects.emplace_back(ModelObject::new_copy(*model.objects[i]));
				m_model.objects.back()->set_model(&m_model);
            }
            //BBS: add more logs
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: new model objects added.")%__LINE__;
        } else {
            //BBS: add more logs
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: model object changed.")%__LINE__;
            // Reorder the objects, add new objects.
            // First stop background processing before shuffling or deleting the PrintObjects in the object list.
            this->call_cancel_callback();
            //BBS: we don't need to set invalid here, we judge it by comparing the print_object list
            //update_apply_status(this->invalidate_step(psGCodeExport));
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
                    model_object_status_db.add(*mobj, ModelObjectStatus::New);
                } else {
                    // Existing ModelObject re-added (possibly moved in the list).
                    m_model.objects.emplace_back(*it);
                    model_object_status_db.add(*mobj, ModelObjectStatus::Moved);
                }
            }
            bool deleted_any = false;
			for (ModelObject *&model_object : model_objects_old)
                if (model_object_status_db.add_if_new(*model_object, ModelObjectStatus::Deleted))
                    deleted_any = true;
                else
                    // Do not delete this ModelObject instance.
                    model_object = nullptr;
            if (deleted_any) {
                // Delete PrintObjects of the deleted ModelObjects.
                PrintObjectPtrs print_objects_old = std::move(m_objects);
                m_objects.clear();
                m_objects.reserve(print_objects_old.size());
                for (PrintObject *print_object : print_objects_old) {
                    const ModelObjectStatus &status = model_object_status_db.get(*print_object->model_object());
                    if (status.status == ModelObjectStatus::Deleted) {
                        update_apply_status(print_object->invalidate_all_steps());
                        delete print_object;
                    } else
                        m_objects.emplace_back(print_object);
                }
                for (ModelObject *model_object : model_objects_old)
                    delete model_object;
                print_regions_reshuffled = true;
            }
        }
    }

    // 2) Map print objects including their transformation matrices.
    PrintObjectStatusDB print_object_status_db(m_objects);

    // 3) Synchronize ModelObjects & PrintObjects.
    const std::initializer_list<ModelVolumeType> solid_or_modifier_types { ModelVolumeType::MODEL_PART, ModelVolumeType::NEGATIVE_VOLUME, ModelVolumeType::PARAMETER_MODIFIER };
    for (size_t idx_model_object = 0; idx_model_object < model.objects.size(); ++ idx_model_object) {
        ModelObject       &model_object        = *m_model.objects[idx_model_object];
        ModelObjectStatus &model_object_status = const_cast<ModelObjectStatus&>(model_object_status_db.reuse(model_object));
		const ModelObject &model_object_new    = *model.objects[idx_model_object];
        if (model_object_status.status == ModelObjectStatus::New)
            // PrintObject instances will be added in the next loop.
            continue;
        // Update the ModelObject instance, possibly invalidate the linked PrintObjects.
        assert(model_object_status.status == ModelObjectStatus::Old || model_object_status.status == ModelObjectStatus::Moved);
        // Check whether a model part volume was added or removed, their transformations or order changed.
        // Only volume IDs, volume types, transformation matrices and their order are checked, configuration and other parameters are NOT checked.
        bool solid_or_modifier_differ   = model_volume_list_changed(model_object, model_object_new, solid_or_modifier_types) ||
                                          model_mmu_segmentation_data_changed(model_object, model_object_new) ||
                                          (model_object_new.is_mm_painted() && num_extruders_changed) ||
                                          model_fuzzy_skin_data_changed(model_object, model_object_new);
        bool supports_differ            = model_volume_list_changed(model_object, model_object_new, ModelVolumeType::SUPPORT_BLOCKER) ||
                                          model_volume_list_changed(model_object, model_object_new, ModelVolumeType::SUPPORT_ENFORCER);
        bool layer_height_ranges_differ = ! layer_height_ranges_equal(model_object.layer_config_ranges, model_object_new.layer_config_ranges, model_object_new.layer_height_profile.empty());
        bool model_origin_translation_differ = model_object.origin_translation != model_object_new.origin_translation;
        bool brim_points_differ = model_brim_points_data_changed(model_object, model_object_new);
        auto print_objects_range        = print_object_status_db.get_range(model_object);
        // The list actually can be empty if all instances are out of the print bed.
        //assert(print_objects_range.begin() != print_objects_range.end());
        // All PrintObjects in print_objects_range shall point to the same prints_objects_regions
        if (print_objects_range.begin() != print_objects_range.end()) {
            model_object_status.print_object_regions = print_objects_range.begin()->print_object->m_shared_regions;
            model_object_status.print_object_regions->ref_cnt_inc();
        }
        if (solid_or_modifier_differ || model_origin_translation_differ || layer_height_ranges_differ ||
            ! model_object.layer_height_profile.timestamp_matches(model_object_new.layer_height_profile)) {
            // The very first step (the slicing step) is invalidated. One may freely remove all associated PrintObjects.
            model_object_status.print_object_regions_status =
                model_object_status.print_object_regions == nullptr || model_origin_translation_differ || layer_height_ranges_differ ?
                // Drop print_objects_regions.
                ModelObjectStatus::PrintObjectRegionsStatus::Invalid :
                // Reuse bounding boxes of print_objects_regions for ModelVolumes with unmodified transformation.
                ModelObjectStatus::PrintObjectRegionsStatus::PartiallyValid;
            for (const PrintObjectStatus &print_object_status : print_objects_range) {
                update_apply_status(print_object_status.print_object->invalidate_all_steps());
                const_cast<PrintObjectStatus&>(print_object_status).status = PrintObjectStatus::Deleted;
            }
            if (model_object_status.print_object_regions_status == ModelObjectStatus::PrintObjectRegionsStatus::PartiallyValid)
                // Drop everything from PrintObjectRegions but those VolumeExtents (of their particular ModelVolumes) that are still valid.
                print_objects_regions_invalidate_keep_some_volumes(*model_object_status.print_object_regions, model_object.volumes, model_object_new.volumes);
            else if (model_object_status.print_object_regions != nullptr)
                model_object_status.print_object_regions->clear();
            // Copy content of the ModelObject including its ID, do not change the parent.
            model_object.assign_copy(model_object_new);
        } else {
            model_object_status.print_object_regions_status = ModelObjectStatus::PrintObjectRegionsStatus::Valid;
            if (supports_differ || model_custom_supports_data_changed(model_object, model_object_new)) {
                // First stop background processing before shuffling or deleting the ModelVolumes in the ModelObject's list.
                if (supports_differ) {
                    this->call_cancel_callback();
                    update_apply_status(false);
                }
                // Invalidate just the supports step.
                for (const PrintObjectStatus &print_object_status : print_objects_range)
                    update_apply_status(print_object_status.print_object->invalidate_step(posSupportMaterial));
                if (supports_differ) {
                    // Copy just the support volumes.
                    model_volume_list_update_supports(model_object, model_object_new);
                }
            } else if (model_custom_seam_data_changed(model_object, model_object_new)) {
                update_apply_status(this->invalidate_step(psGCodeExport));
            }
            if (brim_points_differ) {
                model_object.brim_points = model_object_new.brim_points;
                update_apply_status(this->invalidate_all_steps());
            }
        }
        if (! solid_or_modifier_differ) {
            // Synchronize Object's config.
            bool object_config_changed = ! model_object.config.timestamp_matches(model_object_new.config);
			if (object_config_changed)
				model_object.config.assign_config(model_object_new.config);
            if (! object_diff.empty() || object_config_changed || num_extruders_changed ) {
                PrintObjectConfig new_config = PrintObject::object_config_from_model_object(m_default_object_config, model_object, num_extruders );
                for (const PrintObjectStatus &print_object_status : print_object_status_db.get_range(model_object)) {
                    t_config_option_keys diff = print_object_status.print_object->config().diff(new_config);
                    if (! diff.empty()) {
                        update_apply_status(print_object_status.print_object->invalidate_state_by_config_options(print_object_status.print_object->config(), new_config, diff));
                        print_object_status.print_object->config_apply_only(new_config, diff, true);
                    }
                }
            }
            // Synchronize (just copy) the remaining data of ModelVolumes (name, config, custom supports data).
            //FIXME What to do with m_material_id?
			model_volume_list_copy_configs(model_object /* dst */, model_object_new /* src */, ModelVolumeType::MODEL_PART);
			model_volume_list_copy_configs(model_object /* dst */, model_object_new /* src */, ModelVolumeType::PARAMETER_MODIFIER);
            layer_height_ranges_copy_configs(model_object.layer_config_ranges /* dst */, model_object_new.layer_config_ranges /* src */);
            // Copy the ModelObject name, input_file and instances. The instances will be compared against PrintObject instances in the next step.
            model_object.name       = model_object_new.name;
            model_object.input_file = model_object_new.input_file;
            // Only refresh ModelInstances if there is any change.
            if (model_object.instances.size() != model_object_new.instances.size() ||
            	! std::equal(model_object.instances.begin(), model_object.instances.end(), model_object_new.instances.begin(), [](auto l, auto r){ return l->id() == r->id(); })) {
            	// G-code generator accesses model_object.instances to generate sequential print ordering matching the Plater object list.
            	update_apply_status(this->invalidate_step(psGCodeExport));
	            model_object.clear_instances();
	            model_object.instances.reserve(model_object_new.instances.size());
	            for (const ModelInstance *model_instance : model_object_new.instances) {
	                model_object.instances.emplace_back(new ModelInstance(*model_instance));
	                model_object.instances.back()->set_model_object(&model_object);
	            }
	        } else if (! std::equal(model_object.instances.begin(), model_object.instances.end(), model_object_new.instances.begin(),
	        		[](auto l, auto r){ return l->print_volume_state == r->print_volume_state && l->printable == r->printable &&
	        						           l->get_transformation().get_matrix().isApprox(r->get_transformation().get_matrix()); })) {
	        	// If some of the instances changed, the bounding box of the updated ModelObject is likely no more valid.
	        	// This is safe as the ModelObject's bounding box is only accessed from this function, which is called from the main thread only.
	 			model_object.invalidate_bounding_box();
	        	// Synchronize the content of instances.
	        	auto new_instance = model_object_new.instances.begin();
				for (auto old_instance = model_object.instances.begin(); old_instance != model_object.instances.end(); ++ old_instance, ++ new_instance) {
                    if (is_printable_filament_changed(new_full_config, (*old_instance)->convex_hull_2d(), (*new_instance)->convex_hull_2d())) {
                        update_apply_status(this->invalidate_steps({psWipeTower, psGCodeExport}));
                    }
                    (*old_instance)->set_transformation((*new_instance)->get_transformation());
                    (*old_instance)->print_volume_state = (*new_instance)->print_volume_state;
                    (*old_instance)->printable 		    = (*new_instance)->printable;
  				}
	        }
        }
    }

    // 4) Generate PrintObjects from ModelObjects and their instances.
    {
        PrintObjectPtrs print_objects_new;
        print_objects_new.reserve(std::max(m_objects.size(), m_model.objects.size()));
        bool new_objects = false;
        // Walk over all new model objects and check, whether there are matching PrintObjects.
        for (ModelObject *model_object : m_model.objects) {
            ModelObjectStatus &model_object_status = const_cast<ModelObjectStatus&>(model_object_status_db.reuse(*model_object));
            // Orca: Updated for XYZ filament shrink compensation
            model_object_status.print_instances = print_objects_from_model_object(*model_object, this->shrinkage_compensation());
            std::vector<const PrintObjectStatus*> old;
            old.reserve(print_object_status_db.count(*model_object));
            for (const PrintObjectStatus &print_object_status : print_object_status_db.get_range(*model_object))
                if (print_object_status.status != PrintObjectStatus::Deleted)
                    old.emplace_back(&print_object_status);
            // Generate a list of trafos and XY offsets for instances of a ModelObject
            // Producing the config for PrintObject on demand, caching it at print_object_last.
            const PrintObject *print_object_last = nullptr;
            auto print_object_apply_config = [this, &print_object_last, model_object, num_extruders ](PrintObject *print_object) {
                print_object->config_apply(print_object_last ?
                    print_object_last->config() :
                    PrintObject::object_config_from_model_object(m_default_object_config, *model_object, num_extruders ));
                print_object_last = print_object;
            };
            if (old.empty()) {
                // Simple case, just generate new instances.
                for (PrintObjectTrafoAndInstances &print_instances : model_object_status.print_instances) {
                    PrintObject *print_object = new PrintObject(this, model_object, print_instances.trafo, std::move(print_instances.instances));
                    print_object_apply_config(print_object);
                    print_objects_new.emplace_back(print_object);
                    // print_object_status.emplace(PrintObjectStatus(print_object, PrintObjectStatus::New));
                    new_objects = true;
                }
                continue;
            }
            // Complex case, try to merge the two lists.
            // Sort the old lexicographically by their trafos.
            std::sort(old.begin(), old.end(), [](const PrintObjectStatus *lhs, const PrintObjectStatus *rhs){ return transform3d_lower(lhs->trafo, rhs->trafo); });
            // Merge the old / new lists.
            auto it_old = old.begin();
            for (PrintObjectTrafoAndInstances &new_instances : model_object_status.print_instances) {
				for (; it_old != old.end() && transform3d_lower((*it_old)->trafo, new_instances.trafo); ++ it_old);
				if (it_old == old.end() || ! transform3d_equal((*it_old)->trafo, new_instances.trafo)) {
                    // This is a new instance (or a set of instances with the same trafo). Just add it.
                    PrintObject *print_object = new PrintObject(this, model_object, new_instances.trafo, std::move(new_instances.instances));
                    print_object_apply_config(print_object);
                    print_objects_new.emplace_back(print_object);
                    // print_object_status.emplace(PrintObjectStatus(print_object, PrintObjectStatus::New));
                    new_objects = true;
                    if (it_old != old.end())
                        const_cast<PrintObjectStatus*>(*it_old)->status = PrintObjectStatus::Deleted;
                } else {
                    // The PrintObject already exists and the copies differ.
					PrintBase::ApplyStatus status = (*it_old)->print_object->set_instances(std::move(new_instances.instances));
                    if (status != PrintBase::APPLY_STATUS_UNCHANGED) {
                        size_t extruder_num = new_full_config.option<ConfigOptionFloats>("nozzle_diameter")->size();
                        update_apply_status(status == PrintBase::APPLY_STATUS_INVALIDATED);
                    }
					print_objects_new.emplace_back((*it_old)->print_object);
					const_cast<PrintObjectStatus*>(*it_old)->status = PrintObjectStatus::Reused;
				}
            }
        }
        if (m_objects != print_objects_new) {
            //BBS: add more logs
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: found print object changed.")%__LINE__;
            this->call_cancel_callback();
			update_apply_status(this->invalidate_all_steps());
            m_objects = print_objects_new;
            // Delete the PrintObjects marked as Unknown or Deleted.
            bool deleted_objects = false;
            for (const PrintObjectStatus &pos : print_object_status_db)
                if (pos.status == PrintObjectStatus::Unknown || pos.status == PrintObjectStatus::Deleted) {
                    update_apply_status(pos.print_object->invalidate_all_steps());
                    delete pos.print_object;
					deleted_objects = true;
                }
			if (new_objects || deleted_objects)
                update_apply_status(this->invalidate_steps({ psSkirtBrim, psWipeTower, psGCodeExport }));
			if (new_objects)
	            update_apply_status(false);
            print_regions_reshuffled = true;
        }
        print_object_status_db.clear();

        // BBS
        for (PrintObject* object : m_objects) {
            auto ept_iter = std::find(print_diff.begin(), print_diff.end(), "enable_prime_tower");
            if (/*object->config().adaptive_layer_height &&*/ ept_iter != print_diff.end()) {
                update_apply_status(object->invalidate_step(posSlice));
            }
        }
    }

    //BBS: check the config again
    int new_used_filaments = this->extruders(true).size();
    t_config_option_keys new_changed_keys = new_full_config.normalize_fdm_2(objects().size(), new_used_filaments);
    if (new_changed_keys.size() > 0) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", got new_changed_keys, size=%1%")%new_changed_keys.size();
        for (int i = 0; i < new_changed_keys.size(); i++)
        {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", i=%1%, key=%2%")%i %new_changed_keys[i];
        }

        update_apply_status(false);

        // The following call may stop the background processing.
        update_apply_status(this->invalidate_state_by_config_options(new_full_config, new_changed_keys));

        update_apply_status(this->invalidate_step(psGCodeExport));

        if (full_config_diff.empty()) {
            //BBS: previous empty
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: full_config_diff previous empty, need to apply now.")%__LINE__;

            m_placeholder_parser.clear_config();
            // Set the profile aliases for the PrintBase::output_filename()
            m_placeholder_parser.set("print_preset",              new_full_config.option("print_settings_id")->clone());
            m_placeholder_parser.set("filament_preset",           new_full_config.option("filament_settings_id")->clone());
            m_placeholder_parser.set("printer_preset",            new_full_config.option("printer_settings_id")->clone());

            //m_placeholder_parser.apply_config(filament_overrides);
        }
        // It is also safe to change m_config now after this->invalidate_state_by_config_options() call.
        m_config.apply_only(new_full_config, new_changed_keys, true);
        // Handle changes to object config defaults
        m_default_object_config.apply_only(new_full_config, new_changed_keys, true);
        // Handle changes to regions config defaults
        m_default_region_config.apply_only(new_full_config, new_changed_keys, true);
        m_full_print_config = std::move(new_full_config);
    }

    // All regions now have distinct settings.
    // Check whether applying the new region config defaults we would get different regions,
    // update regions or create regions from scratch.
    for (auto it_print_object = m_objects.begin(); it_print_object != m_objects.end();) {
        // Find the range of PrintObjects sharing the same associated ModelObject.
        auto                it_print_object_end  = it_print_object;
        PrintObject        &print_object         = *(*it_print_object);
        const ModelObject  &model_object         = *print_object.model_object();
        ModelObjectStatus  &model_object_status  = const_cast<ModelObjectStatus&>(model_object_status_db.reuse(model_object));
        PrintObjectRegions *print_object_regions = model_object_status.print_object_regions;
        for (++ it_print_object_end; it_print_object_end != m_objects.end() && (*it_print_object)->model_object() == (*it_print_object_end)->model_object(); ++ it_print_object_end)
            assert((*it_print_object_end)->m_shared_regions == nullptr || (*it_print_object_end)->m_shared_regions == print_object_regions);
        if (print_object_regions == nullptr) {
            print_object_regions = new PrintObjectRegions{};
            model_object_status.print_object_regions = print_object_regions;
            print_object_regions->ref_cnt_inc();
        }
        std::vector<unsigned int> painting_extruders;
        if (const auto &volumes = print_object.model_object()->volumes;
            num_extruders > 1 &&
            std::find_if(volumes.begin(), volumes.end(), [](const ModelVolume *v) { return ! v->mmu_segmentation_facets.empty(); }) != volumes.end()) {

            std::array<bool, static_cast<size_t>(EnforcerBlockerType::ExtruderMax) + 1> used_facet_states{};
            for (const ModelVolume *volume : volumes) {
                const std::vector<bool> &volume_used_facet_states = volume->mmu_segmentation_facets.get_data().used_states;

                assert(volume_used_facet_states.size() == used_facet_states.size());
                for (size_t state_idx = 0; state_idx < std::min(volume_used_facet_states.size(), used_facet_states.size()); ++state_idx)
                    used_facet_states[state_idx] |= volume_used_facet_states[state_idx];
            }

            for (size_t state_idx = static_cast<size_t>(EnforcerBlockerType::Extruder1); state_idx < used_facet_states.size(); ++state_idx) {
                if (used_facet_states[state_idx])
                    painting_extruders.emplace_back(state_idx);
            }
        }
        if (model_object_status.print_object_regions_status == ModelObjectStatus::PrintObjectRegionsStatus::Valid) {
            // Verify that the trafo for regions & volume bounding boxes thus for regions is still applicable.
            auto invalidate = [it_print_object, it_print_object_end, update_apply_status]() {
                for (auto it = it_print_object; it != it_print_object_end; ++ it)
                    if ((*it)->m_shared_regions != nullptr)
                        update_apply_status((*it)->invalidate_all_steps());
            };
            if (print_object_regions && ! trafos_differ_in_rotation_by_z_and_mirroring_by_xy_only(print_object_regions->trafo_bboxes, model_object_status.print_instances.front().trafo)) {
                invalidate();
                print_object_regions->clear();
                model_object_status.print_object_regions_status = ModelObjectStatus::PrintObjectRegionsStatus::Invalid;
                print_regions_reshuffled = true;
            } else if (print_object_regions &&
                verify_update_print_object_regions(
                    print_object.model_object()->volumes,
                    m_default_region_config,
                    num_extruders,
                    *print_object_regions,
                    [it_print_object, it_print_object_end, &update_apply_status](const PrintRegionConfig &old_config, const PrintRegionConfig &new_config, const t_config_option_keys &diff_keys) {
                        for (auto it = it_print_object; it != it_print_object_end; ++it)
                            if ((*it)->m_shared_regions != nullptr)
                                update_apply_status((*it)->invalidate_state_by_config_options(old_config, new_config, diff_keys));
                    })) {
                // Regions are valid, just keep them.
            } else {
                // Regions were reshuffled.
                invalidate();
                // At least reuse layer ranges and bounding boxes of ModelVolumes.
                model_object_status.print_object_regions_status = ModelObjectStatus::PrintObjectRegionsStatus::PartiallyValid;
                print_regions_reshuffled = true;
            }
        }
        if (print_object_regions == nullptr || model_object_status.print_object_regions_status != ModelObjectStatus::PrintObjectRegionsStatus::Valid) {
            // Layer ranges with their associated configurations. Remove overlaps between the ranges
            // and create the regions from scratch.
            print_object_regions = generate_print_object_regions(
                print_object_regions,
                print_object.model_object()->volumes,
                LayerRanges(print_object.model_object()->layer_config_ranges),
                m_default_region_config,
                model_object_status.print_instances.front().trafo,
                num_extruders ,
                print_object.is_mm_painted() ? 0.f : float(print_object.config().xy_contour_compensation.value),
                painting_extruders,
                print_object.is_fuzzy_skin_painted());
        }
        for (auto it = it_print_object; it != it_print_object_end; ++it)
            if ((*it)->m_shared_regions) {
                assert((*it)->m_shared_regions == print_object_regions);
            } else {
                (*it)->m_shared_regions = print_object_regions;
                print_object_regions->ref_cnt_inc();
            }
        it_print_object = it_print_object_end;
    }

    if (print_regions_reshuffled) {
        // Update Print::m_print_regions from objects.
        struct cmp { bool operator() (const PrintRegion *l, const PrintRegion *r) const { return l->config_hash() == r->config_hash() && l->config() == r->config(); } };
        std::set<const PrintRegion*, cmp> region_set;
        m_print_regions.clear();
        PrintObjectRegions *print_object_regions = nullptr;
        for (PrintObject *print_object : m_objects)
            if (print_object_regions != print_object->m_shared_regions) {
                print_object_regions = print_object->m_shared_regions;
                for (std::unique_ptr<Slic3r::PrintRegion> &print_region : print_object_regions->all_regions)
                    if (auto it = region_set.find(print_region.get()); it == region_set.end()) {
                        int print_region_id = int(m_print_regions.size());
                        m_print_regions.emplace_back(print_region.get());
                        print_region->m_print_region_id = print_region_id;
                    } else {
                        print_region->m_print_region_id = (*it)->print_region_id();
                    }
            }
    }

    // Update SlicingParameters for each object where the SlicingParameters is not valid.
    // If it is not valid, then it is ensured that PrintObject.m_slicing_params is not in use
    // (posSlicing and posSupportMaterial was invalidated).
    for (PrintObject *object : m_objects)
    {
        object->update_slicing_parameters();
        m_support_used |= object->config().enable_support;
    }

#ifdef _DEBUG
    check_model_ids_equal(m_model, model);
#endif /* _DEBUG */

	//BBS: add timestamp logic
	if (apply_status != APPLY_STATUS_UNCHANGED)
		m_modified_count++;
	BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: finished,  this %2%, m_modified_count %3%, apply_status %4%, m_support_used %5%")%__LINE__ %this %m_modified_count %apply_status %m_support_used;
	return static_cast<ApplyStatus>(apply_status);
}

} // namespace Slic3r
