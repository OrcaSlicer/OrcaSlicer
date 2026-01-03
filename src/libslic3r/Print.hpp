#ifndef slic3r_Print_hpp_
#define slic3r_Print_hpp_

#include "PrintBase.hpp"
#include "Fill/FillAdaptive.hpp"
#include "Fill/FillLightning.hpp"

#include "BoundingBox.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Flow.hpp"
#include "Point.hpp"
#include "Slicing.hpp"
#include "TriangleMeshSlicer.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/WipeTower.hpp"
#include "GCode/WipeTower2.hpp"
#include "GCode/ThumbnailData.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "MultiMaterialSegmentation.hpp"
#include "libslic3r.h"

#include <Eigen/Geometry>

#include <functional>
#include <set>

#include "calib.hpp"

namespace Slic3r {

class GCode;
class Layer;
class ModelObject;
class Print;
class PrintObject;
class SupportLayer;
// BBS
class TreeSupportData;
class TreeSupport;
class ExtrusionLayers;

#define MAX_OUTER_NOZZLE_DIAMETER   4
// BBS: move from PrintObjectSlice.cpp
struct VolumeSlices
{
    ObjectID                volume_id;
    std::vector<ExPolygons> slices;
};

struct groupedVolumeSlices
{
    int                     groupId = -1;
    std::vector<ObjectID>   volume_ids;
    ExPolygons              slices;
};

enum SupportNecessaryType {
    NoNeedSupp=0,
    SharpTail,
    Cantilever,
    LargeOverhang,
};

namespace FillAdaptive {
    struct Octree;
    struct OctreeDeleter;
    using OctreePtr = std::unique_ptr<Octree, OctreeDeleter>;
};

namespace FillLightning {
    class Generator;
    struct GeneratorDeleter;
    using GeneratorPtr = std::unique_ptr<Generator, GeneratorDeleter>;
}; // namespace FillLightning

// Print step IDs for keeping track of the print state.
// The Print steps are applied in this order.
enum PrintStep {
    psWipeTower,
    // Ordering of the tools on PrintObjects for a multi-material print.
    // psToolOrdering is a synonym to psWipeTower, as the Wipe Tower calculates and modifies the ToolOrdering,
    // while if printing without the Wipe Tower, the ToolOrdering is calculated as well.
    psToolOrdering = psWipeTower,
    psSkirtBrim,
    // Last step before G-code export, after this step is finished, the initial extrusion path preview
    // should be refreshed.
    psSlicingFinished = psSkirtBrim,
    psGCodeExport,
    psConflictCheck,
    psCount
};

enum PrintObjectStep {
    posSlice, posPerimeters,posEstimateCurledExtrusions, posPrepareInfill,
    posInfill, posIroning, posSupportMaterial, posSimplifyPath, posSimplifySupportPath,
    // BBS
    posDetectOverhangsForLift,
    posSimplifyWall, posSimplifyInfill,
    posCount,
};

// A PrintRegion object represents a group of volumes to print
// sharing the same config (including the same assigned extruder(s))
class PrintRegion
{
public:
    PrintRegion() = default;
    PrintRegion(const PrintRegionConfig &config);
    PrintRegion(const PrintRegionConfig &config, const size_t config_hash, int print_object_region_id = -1) : m_config(config), m_config_hash(config_hash), m_print_object_region_id(print_object_region_id) {}
    PrintRegion(PrintRegionConfig &&config);
    PrintRegion(PrintRegionConfig &&config, const size_t config_hash, int print_object_region_id = -1) : m_config(std::move(config)), m_config_hash(config_hash), m_print_object_region_id(print_object_region_id) {}
    ~PrintRegion() = default;

// Methods NOT modifying the PrintRegion's state:
public:
    const PrintRegionConfig&    config() const throw() { return m_config; }
    size_t                      config_hash() const throw() { return m_config_hash; }
    // Identifier of this PrintRegion in the list of Print::m_print_regions.
    int                         print_region_id() const throw() { return m_print_region_id; }
    int                         print_object_region_id() const throw() { return m_print_object_region_id; }
	// 1-based extruder identifier for this region and role.
	unsigned int 				extruder(FlowRole role) const;
    Flow                        flow(const PrintObject &object, FlowRole role, double layer_height, bool first_layer = false) const;
    // Average diameter of nozzles participating on extruding this region.
    coordf_t                    nozzle_dmr_avg(const PrintConfig &print_config) const;
    // Average diameter of nozzles participating on extruding this region.
    coordf_t                    bridging_height_avg(const PrintConfig &print_config) const;

    // Collect 0-based extruder indices used to print this region's object.
	void                        collect_object_printing_extruders(const Print &print, std::vector<unsigned int> &object_extruders) const;
	static void                 collect_object_printing_extruders(const PrintConfig &print_config, const PrintRegionConfig &region_config, const bool has_brim, std::vector<unsigned int> &object_extruders);

// Methods modifying the PrintRegion's state:
public:
    void                        set_config(const PrintRegionConfig &config) { m_config = config; m_config_hash = m_config.hash(); }
    void                        set_config(PrintRegionConfig &&config) { m_config = std::move(config); m_config_hash = m_config.hash(); }
    void                        config_apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent = false)
                                        { m_config.apply_only(other, keys, ignore_nonexistent); m_config_hash = m_config.hash(); }
private:
    friend Print;
    friend void print_region_ref_inc(PrintRegion&);
    friend void print_region_ref_reset(PrintRegion&);
    friend int  print_region_ref_cnt(const PrintRegion&);

    PrintRegionConfig  m_config;
    size_t             m_config_hash;
    int                m_print_region_id { -1 };
    int                m_print_object_region_id { -1 };
    int                m_ref_cnt { 0 };
};

inline bool operator==(const PrintRegion &lhs, const PrintRegion &rhs) { return lhs.config_hash() == rhs.config_hash() && lhs.config() == rhs.config(); }
inline bool operator!=(const PrintRegion &lhs, const PrintRegion &rhs) { return ! (lhs == rhs); }

template<typename T>
class ConstVectorOfPtrsAdaptor {
public:
    // Returning a non-const pointer to const pointers to T.
    T * const *             begin() const { return m_data->data(); }
    T * const *             end()   const { return m_data->data() + m_data->size(); }
    const T*                front() const { return m_data->front(); }
    // BBS
    const T*                back()  const { return m_data->back(); }
    size_t                  size()  const { return m_data->size(); }
    bool                    empty() const { return m_data->empty(); }
    const T*                operator[](size_t i) const { return (*m_data)[i]; }
    const T*                at(size_t i) const { return m_data->at(i); }
    std::vector<const T*>   vector() const { return std::vector<const T*>(this->begin(), this->end()); }
protected:
    ConstVectorOfPtrsAdaptor(const std::vector<T*> *data) : m_data(data) {}
private:
    const std::vector<T*> *m_data;
};

typedef std::vector<Layer*>       LayerPtrs;
typedef std::vector<const Layer*> ConstLayerPtrs;
class ConstLayerPtrsAdaptor : public ConstVectorOfPtrsAdaptor<Layer> {
    friend PrintObject;
    ConstLayerPtrsAdaptor(const LayerPtrs *data) : ConstVectorOfPtrsAdaptor<Layer>(data) {}
};

typedef std::vector<SupportLayer*>        SupportLayerPtrs;
typedef std::vector<const SupportLayer*>  ConstSupportLayerPtrs;
class ConstSupportLayerPtrsAdaptor : public ConstVectorOfPtrsAdaptor<SupportLayer> {
    friend PrintObject;
    ConstSupportLayerPtrsAdaptor(const SupportLayerPtrs *data) : ConstVectorOfPtrsAdaptor<SupportLayer>(data) {}
};

// Single instance of a PrintObject.
// As multiple PrintObjects may be generated for a single ModelObject (their instances differ in rotation around Z),
// ModelObject's instancess will be distributed among these multiple PrintObjects.
struct PrintInstance
{
    // Parent PrintObject
    PrintObject 		*print_object;
    // Source ModelInstance of a ModelObject, for which this print_object was created.
	const ModelInstance *model_instance;
	// Shift of this instance's center into the world coordinates.
	Point 				 shift;
    
    BoundingBoxf3   get_bounding_box() const;
    Polygon get_convex_hull_2d();
    // SoftFever
    // 
    // instance id
    size_t               id;
    // Orca: unique id used by marlin/rrf cancel object feature
    size_t               unique_id;

    //BBS: instance_shift is too large because of multi-plate, apply without plate offset.
    Point shift_without_plate_offset() const;
};

typedef std::vector<PrintInstance> PrintInstances;

class PrintObjectRegions
{
public:
    // Bounding box of a ModelVolume transformed into the working space of a PrintObject, possibly
    // clipped by a layer range modifier.
    // Only Eigen types of Nx16 size are vectorized. This bounding box will not be vectorized.
    static_assert(sizeof(Eigen::AlignedBox<float, 3>) == 24, "Eigen::AlignedBox<float, 3> is not being vectorized, thus it does not need to be aligned");
    using BoundingBox = Eigen::AlignedBox<float, 3>;
    struct VolumeExtents {
        ObjectID             volume_id;
        BoundingBox          bbox;
    };

    struct VolumeRegion
    {
        // ID of the associated ModelVolume.
        const ModelVolume   *model_volume { nullptr };
        // Index of a parent VolumeRegion.
        int                  parent { -1 };
        // Pointer to PrintObjectRegions::all_regions, null for a negative volume.
        PrintRegion         *region { nullptr };
        // Pointer to VolumeExtents::bbox.
        const BoundingBox   *bbox { nullptr };
        // To speed up merging of same regions.
        const VolumeRegion  *prev_same_region { nullptr };
    };

    struct PaintedRegion
    {
        // 1-based extruder identifier.
        unsigned int     extruder_id;
        // Index of a parent VolumeRegion.
        int              parent { -1 };
        // Pointer to PrintObjectRegions::all_regions.
        PrintRegion     *region { nullptr };
    };

    struct LayerRangeRegions;

    struct FuzzySkinPaintedRegion
    {
        enum class ParentType { VolumeRegion, PaintedRegion };

        ParentType   parent_type { ParentType::VolumeRegion };
        // Index of a parent VolumeRegion or PaintedRegion.
        int          parent { -1 };
        // Pointer to PrintObjectRegions::all_regions.
        PrintRegion *region { nullptr };

        PrintRegion *parent_print_object_region(const LayerRangeRegions &layer_range) const;
        int          parent_print_object_region_id(const LayerRangeRegions &layer_range) const;
    };

    // One slice over the PrintObject (possibly the whole PrintObject) and a list of ModelVolumes and their bounding boxes
    // possibly clipped by the layer_height_range.
    struct LayerRangeRegions
    {
        t_layer_height_range        layer_height_range;
        // Config of the layer range, null if there is just a single range with no config override.
        // Config is owned by the associated ModelObject.
        const DynamicPrintConfig*   config { nullptr };
        // Volumes sorted by ModelVolume::id().
        std::vector<VolumeExtents>  volumes;

        // Sorted in the order of their source ModelVolumes, thus reflecting the order of region clipping, modifier overrides etc.
        std::vector<VolumeRegion>           volume_regions;
        std::vector<PaintedRegion>          painted_regions;
        std::vector<FuzzySkinPaintedRegion> fuzzy_skin_painted_regions;

        bool has_volume(const ObjectID id) const {
            auto it = lower_bound_by_predicate(this->volumes.begin(), this->volumes.end(), [id](const VolumeExtents &l) { return l.volume_id < id; });
            return it != this->volumes.end() && it->volume_id == id;
        }
    };

    std::vector<std::unique_ptr<PrintRegion>>   all_regions;
    std::vector<LayerRangeRegions>              layer_ranges;
    // Transformation of this ModelObject into one of the associated PrintObjects (all PrintObjects derived from a single modelObject differ by a Z rotation only).
    // This transformation is used to calculate VolumeExtents.
    Transform3d                                 trafo_bboxes;
    std::vector<ObjectID>                       cached_volume_ids;

    void ref_cnt_inc() { ++ m_ref_cnt; }
    void ref_cnt_dec() { if (-- m_ref_cnt == 0) delete this; }
    void clear() {
        all_regions.clear();
        layer_ranges.clear();
        cached_volume_ids.clear();
    }

private:
    friend class PrintObject;
    // Number of PrintObjects generated from the same ModelObject and sharing the regions.
    // ref_cnt could only be modified by the main thread, thus it does not need to be atomic.
    size_t                                      m_ref_cnt{ 0 };
};

class PrintObject : public PrintObjectBaseWithState<Print, PrintObjectStep, posCount>
{
private: // Prevents erroneous use by other classes.
    typedef PrintObjectBaseWithState<Print, PrintObjectStep, posCount> Inherited;

public:
    // Size of an object: XYZ in scaled coordinates. The size might not be quite snug in XY plane.
    const Vec3crd&               size() const			{ return m_size; }
    const PrintObjectConfig&     config() const         { return m_config; }
    void                         configBrimWidth(double m)      {m_config.brim_width.value = m; }
    ConstLayerPtrsAdaptor        layers() const         { return ConstLayerPtrsAdaptor(&m_layers); }
    ConstSupportLayerPtrsAdaptor support_layers() const { return ConstSupportLayerPtrsAdaptor(&m_support_layers); }
    const Transform3d&           trafo() const          { return m_trafo; }
    // Trafo with the center_offset() applied after the transformation, to center the object in XY before slicing.
    Transform3d                  trafo_centered() const
        { Transform3d t = this->trafo(); t.pretranslate(Vec3d(- unscale<double>(m_center_offset.x()), - unscale<double>(m_center_offset.y()), 0)); return t; }
    const PrintInstances&        instances() const      { return m_instances; }
    PrintInstances &instances() { return m_instances; }

    // Whoever will get a non-const pointer to PrintObject will be able to modify its layers.
    LayerPtrs&                   layers()               { return m_layers; }
    SupportLayerPtrs&            support_layers()       { return m_support_layers; }

    template<typename PolysType>
    static void remove_bridges_from_contacts(
        const Layer* lower_layer,
        const Layer* current_layer,
        float extrusion_width,
        PolysType* overhang_regions,
        float max_bridge_length = scale_(10),
        bool break_bridge=false);

    // Bounding box is used to align the object infill patterns, and to calculate attractor for the rear seam.
    // The bounding box may not be quite snug.
    BoundingBox                  bounding_box() const   { return BoundingBox(Point(- m_size.x() / 2, - m_size.y() / 2), Point(m_size.x() / 2, m_size.y() / 2)); }
    // Height is used for slicing, for sorting the objects by height for sequential printing and for checking vertical clearence in sequential print mode.
    // The height is snug.
    coord_t 				     height() const         { return m_size.z(); }
    double                      max_z() const         { return m_max_z; }
    // Centering offset of the sliced mesh from the scaled and rotated mesh of the model.
    const Point& 			     center_offset() const  { return m_center_offset; }

    // BBS
    void generate_support_preview();
    const std::vector<VolumeSlices>& firstLayerObjSlice() const { return firstLayerObjSliceByVolume; }
    std::vector<VolumeSlices>& firstLayerObjSliceMod() { return firstLayerObjSliceByVolume; }
    const std::vector<groupedVolumeSlices>& firstLayerObjGroups() const { return firstLayerObjSliceByGroups; }
    std::vector<groupedVolumeSlices>& firstLayerObjGroupsMod() { return firstLayerObjSliceByGroups; }

    bool                         has_brim() const       {
        return ((this->config().brim_type != btNoBrim && this->config().brim_width.value > 0.) || this->config().brim_type == btAutoBrim
            || (this->config().brim_type == btPainted && !this->model_object()->brim_points.empty()))
            && ! this->has_raft();
    }

    // BBS
    const ExtrusionEntityCollection& object_skirt() const {
        return m_skirt;
    }

    // This is the *total* layer count (including support layers)
    // this value is not supposed to be compared with Layer::id
    // since they have different semantics.
    size_t 			total_layer_count() const { return this->layer_count() + this->support_layer_count(); }
    size_t 			layer_count() const { return m_layers.size(); }
    void 			clear_layers();
    const Layer* 	get_layer(int idx) const { return m_layers[idx]; }
    Layer* 			get_layer(int idx) 		 { return m_layers[idx]; }
    // Get a layer exactly at print_z.
    const Layer*	get_layer_at_printz(coordf_t print_z) const;
    Layer*			get_layer_at_printz(coordf_t print_z);
    // Get a layer approximately at print_z.
    const Layer*	get_layer_at_printz(coordf_t print_z, coordf_t epsilon) const;
    Layer*			get_layer_at_printz(coordf_t print_z, coordf_t epsilon);
    int             get_layer_idx_get_printz(coordf_t print_z, coordf_t epsilon);
    // BBS
    const Layer*    get_layer_at_bottomz(coordf_t bottom_z, coordf_t epsilon) const;
    Layer*          get_layer_at_bottomz(coordf_t bottom_z, coordf_t epsilon);

    // Get the first layer approximately bellow print_z.
    const Layer*	get_first_layer_bellow_printz(coordf_t print_z, coordf_t epsilon) const;

    // print_z: top of the layer; slice_z: center of the layer.
    Layer*          add_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z);

    // BBS
    SupportLayer* add_tree_support_layer(int id, coordf_t height, coordf_t print_z, coordf_t slice_z);
    std::shared_ptr<TreeSupportData> alloc_tree_support_preview_cache();
    void clear_tree_support_preview_cache() { m_tree_support_preview_cache.reset(); }

    size_t          support_layer_count() const { return m_support_layers.size(); }
    void            clear_support_layers();
    SupportLayer*   get_support_layer(int idx) { return idx<m_support_layers.size()? m_support_layers[idx]:nullptr; }
    const SupportLayer* get_support_layer_at_printz(coordf_t print_z, coordf_t epsilon) const;
    SupportLayer*   get_support_layer_at_printz(coordf_t print_z, coordf_t epsilon);
    SupportLayer*   add_support_layer(int id, int interface_id, coordf_t height, coordf_t print_z);
    SupportLayerPtrs::iterator insert_support_layer(SupportLayerPtrs::iterator pos, size_t id, size_t interface_id, coordf_t height, coordf_t print_z, coordf_t slice_z);

    // Initialize the layer_height_profile from the model_object's layer_height_profile, from model_object's layer height table, or from slicing parameters.
    // Returns true, if the layer_height_profile was changed.
    static bool     update_layer_height_profile(const ModelObject &model_object, const SlicingParameters &slicing_parameters, std::vector<coordf_t> &layer_height_profile);

    // Collect the slicing parameters, to be used by variable layer thickness algorithm,
    // by the interactive layer height editor and by the printing process itself.
    // The slicing parameters are dependent on various configuration values
    // (layer height, first layer height, raft settings, print nozzle diameter etc).
    const SlicingParameters&    slicing_parameters() const { return m_slicing_params; }
    // Orca: XYZ shrinkage compensation has introduced the const Vec3d &object_shrinkage_compensation parameter to the function below
    static SlicingParameters    slicing_parameters(const DynamicPrintConfig &full_config, const ModelObject &model_object, float object_max_z, const Vec3d &object_shrinkage_compensation);

    size_t                      num_printing_regions() const throw() { return m_shared_regions->all_regions.size(); }
    const PrintRegion&          printing_region(size_t idx) const throw() { return *m_shared_regions->all_regions[idx].get(); }
    //FIXME returing all possible regions before slicing, thus some of the regions may not be slicing at the end.
    std::vector<std::reference_wrapper<const PrintRegion>> all_regions() const;
    const PrintObjectRegions*   shared_regions() const throw() { return m_shared_regions; }

    bool                        has_support()           const { return m_config.enable_support || m_config.enforce_support_layers > 0; }
    bool                        has_raft()              const { return m_config.raft_layers > 0; }
    bool                        has_support_material()  const { return this->has_support() || this->has_raft(); }
    // Checks if the model object is painted using the multi-material painting gizmo.
    bool                        is_mm_painted()         const { return this->model_object()->is_mm_painted(); }
    // Checks if the model object is painted using the fuzzy skin painting gizmo.
    bool                        is_fuzzy_skin_painted() const { return this->model_object()->is_fuzzy_skin_painted(); }

    // returns 0-based indices of extruders used to print the object (without brim, support and other helper extrusions)
    std::vector<unsigned int>   object_extruders() const;

    // Called by make_perimeters()
    void slice();

    // Helpers to slice support enforcer / blocker meshes by the support generator.
    std::vector<Polygons>       slice_support_volumes(const ModelVolumeType model_volume_type) const;
    std::vector<Polygons>       slice_support_blockers() const { return this->slice_support_volumes(ModelVolumeType::SUPPORT_BLOCKER); }
    std::vector<Polygons>       slice_support_enforcers() const { return this->slice_support_volumes(ModelVolumeType::SUPPORT_ENFORCER); }

    // Helpers to project custom facets on slices
    void project_and_append_custom_facets(bool seam, EnforcerBlockerType type, std::vector<Polygons>& expolys, std::vector<std::pair<Vec3f,Vec3f>>* vertical_points=nullptr) const;

    //BBS
    BoundingBox get_first_layer_bbox(float& area, float& layer_height, std::string& name);
    void         get_certain_layers(float start, float end, std::vector<LayerPtrs> &out, std::vector<BoundingBox> &boundingbox_objects);
    Points       get_instances_shift_without_plate_offset() const;
    PrintObject* get_shared_object() const { return m_shared_object; }
    void         set_shared_object(PrintObject *object);
    void         clear_shared_object();
    void         copy_layers_from_shared_object();
    void         copy_layers_overhang_from_shared_object();

    // BBS: Boundingbox of the first layer
    BoundingBox                 firstLayerObjectBrimBoundingBox;

    // BBS: returns 1-based indices of extruders used to print the first layer wall of objects
    std::vector<int>            object_first_layer_wall_extruders;

    // SoftFever
    size_t get_id() const { return m_id; }
    void set_id(size_t id) { m_id = id; }

  private:
    // to be called from Print only.
    friend class Print;

	PrintObject(Print* print, ModelObject* model_object, const Transform3d& trafo, PrintInstances&& instances);
	~PrintObject();

    void                    config_apply(const ConfigBase &other, bool ignore_nonexistent = false) { m_config.apply(other, ignore_nonexistent); }
    void                    config_apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent = false) { m_config.apply_only(other, keys, ignore_nonexistent); }
    PrintBase::ApplyStatus  set_instances(PrintInstances &&instances);
    // Invalidates the step, and its depending steps in PrintObject and Print.
    bool                    invalidate_step(PrintObjectStep step);
    // Invalidates all PrintObject and Print steps.
    bool                    invalidate_all_steps();
    // Invalidate steps based on a set of parameters changed.
    // It may be called for both the PrintObjectConfig and PrintRegionConfig.
    bool                    invalidate_state_by_config_options(
        const ConfigOptionResolver &old_config, const ConfigOptionResolver &new_config, const std::vector<t_config_option_key> &opt_keys);
    // If ! m_slicing_params.valid, recalculate.
    void                    update_slicing_parameters();

    static PrintObjectConfig object_config_from_model_object(const PrintObjectConfig &default_object_config, const ModelObject &object, size_t num_extruders);

private:
    void make_perimeters();
    void prepare_infill();
    void infill();
    void ironing();
    void generate_support_material();
    void estimate_curled_extrusions();
    void simplify_extrusion_path();

    /**
     * @brief Determines the unprintable filaments for each extruder based on its printable area.
     *
     * The returned array will always have the same size as the number of extruders.
     * If extruder num is 1, just return an empty vector.
     * If an extruder has no unprintable filaments, an empty set will also be returned
     *
     * @return A vector of sets representing unprintable filaments for each extruder
     */
    std::vector<std::set<int>> detect_extruder_geometric_unprintables() const;

    void slice_volumes();
    //BBS
    ExPolygons _shrink_contour_holes(double contour_delta, double hole_delta, const ExPolygons& polys) const;
    // BBS
    void detect_overhangs_for_lift();
    void clear_overhangs_for_lift();

   void _transform_hole_to_polyholes();

    // Has any support (not counting the raft).
    void detect_surfaces_type();
    void process_external_surfaces();
    void discover_vertical_shells();
    void bridge_over_infill();
    void clip_fill_surfaces();
    void discover_horizontal_shells();
    void combine_infill();
    void _generate_support_material();
    std::pair<FillAdaptive::OctreePtr, FillAdaptive::OctreePtr> prepare_adaptive_infill_data(
        const std::vector<std::pair<const Surface*, float>>& surfaces_w_bottom_z) const;
    FillLightning::GeneratorPtr prepare_lightning_infill_data();

    // BBS
    SupportNecessaryType is_support_necessary();

    // XYZ in scaled coordinates
    Vec3crd									m_size;
    double                                  m_max_z;
    PrintObjectConfig                       m_config;
    // Translation in Z + Rotation + Scaling / Mirroring.
    Transform3d                             m_trafo = Transform3d::Identity();
    // Slic3r::Point objects in scaled G-code coordinates
    std::vector<PrintInstance>              m_instances;
    // The mesh is being centered before thrown to Clipper, so that the Clipper's fixed coordinates require less bits.
    // This is the adjustment of the  the Object's coordinate system towards PrintObject's coordinate system.
    Point                                   m_center_offset;

    // Object split into layer ranges and regions with their associated configurations.
    // Shared among PrintObjects created for the same ModelObject.
    PrintObjectRegions                     *m_shared_regions { nullptr };

    SlicingParameters                       m_slicing_params;
    LayerPtrs                               m_layers;
    SupportLayerPtrs                        m_support_layers;
    // BBS
    std::shared_ptr<TreeSupportData>        m_tree_support_preview_cache;

    // this is set to true when LayerRegion->slices is split in top/internal/bottom
    // so that next call to make_perimeters() performs a union() before computing loops
    bool                    				m_typed_slices = false;

    std::pair<FillAdaptive::OctreePtr, FillAdaptive::OctreePtr> m_adaptive_fill_octrees;
    FillLightning::GeneratorPtr m_lightning_generator;

    std::vector < VolumeSlices >            firstLayerObjSliceByVolume;
    std::vector<groupedVolumeSlices>        firstLayerObjSliceByGroups;

    // BBS: per object skirt
    ExtrusionEntityCollection               m_skirt;

    PrintObject*                            m_shared_object{ nullptr };

    
    // SoftFever
    // 
    // object id
    size_t               m_id;
    void apply_conical_overhang();

 public:
    //BBS: When printing multi-material objects, this settings will make slicer to clip the overlapping object parts one by the other.
    //(2nd part will be clipped by the 1st, 3rd part will be clipped by the 1st and 2nd etc).
    // This was a per-object setting and now we default enable it.
    static bool clip_multipart_objects;
    static bool infill_only_where_needed;
};

struct FakeWipeTower
{
    // generate fake extrusion
    Vec2f pos;
    float width;
    float height;
    float layer_height;// Due to variable layer height, this parameter may be not right.
    float depth;
    std::vector<std::pair<float, float>> z_and_depth_pairs;
    float brim_width;
    float rotation_angle;
    float cone_angle;
    Vec2d plate_origin;
    Vec2f rib_offset{0.f,0.f};
    std::map<float , Polylines> outer_wall; //wipe tower's true outer wall and brim

    void set_fake_extrusion_data(Vec2f p, float w, float h, float lh, float d, float bd, Vec2d o)
    {
        pos          = p;
        width        = w;
        height       = h;
        layer_height = lh;
        depth        = d;
        brim_width   = bd;
        plate_origin = o;
    }
    void set_fake_extrusion_data(const Vec2f& p, float w, float h, float lh, float d, const std::vector<std::pair<float, float>>& zad, float bd, float ra, float ca, const Vec2d& o)
    {
        pos = p;
        width = w;
        height = h;
        layer_height = lh;
        depth = d;
        z_and_depth_pairs = zad;
        brim_width = bd;
        rotation_angle = ra;
        cone_angle = ca;
        plate_origin = o;
    }

    void set_pos(Vec2f p) { pos = p+rib_offset; }
    void set_pos_and_rotation(const Vec2f& p, float rotation) { pos = p; rotation_angle = rotation; }

    std::vector<ExtrusionPaths> getFakeExtrusionPathsFromWipeTower() const
    {
        int   d         = scale_(depth);
        int   w         = scale_(width);
        int   bd        = scale_(brim_width);
        Point minCorner = {scale_(pos.x()), scale_(pos.y())};
        Point maxCorner = {minCorner.x() + w, minCorner.y() + d};

        std::vector<ExtrusionPaths> paths;
        for (float h = 0.f; h < height; h += layer_height) {
            ExtrusionPath path(ExtrusionRole::erWipeTower, 0.0, 0.0, layer_height);
            path.polyline = {minCorner, {maxCorner.x(), minCorner.y()}, maxCorner, {minCorner.x(), maxCorner.y()}, minCorner};
            paths.push_back({path});

            if (h == 0.f) { // add brim
                ExtrusionPath fakeBrim(ExtrusionRole::erBrim, 0.0, 0.0, layer_height);
                Point         wtbminCorner = {minCorner - Point{bd, bd}};
                Point         wtbmaxCorner = {maxCorner + Point{bd, bd}};
                fakeBrim.polyline          = {wtbminCorner, {wtbmaxCorner.x(), wtbminCorner.y()}, wtbmaxCorner, {wtbminCorner.x(), wtbmaxCorner.y()}, wtbminCorner};
                paths.back().push_back(fakeBrim);
            }
        }
        return paths;
    }

    ExtrusionLayers getTrueExtrusionLayersFromWipeTower() const;

    std::vector<ExtrusionPaths> getFakeExtrusionPathsFromWipeTower2() const
    {
        float h = height;
        float lh = layer_height;
        int   d = scale_(depth);
        int   w = scale_(width);
        int   bd = scale_(brim_width);
        Point minCorner = { -bd, -bd };
        Point maxCorner = { minCorner.x() + w + bd, minCorner.y() + d + bd };

        const auto [cone_base_R, cone_scale_x] = WipeTower2::get_wipe_tower_cone_base(width, height, depth, cone_angle);

        std::vector<ExtrusionPaths> paths;
        for (float hh = 0.f; hh < h; hh += lh) {
            
            if (hh != 0.f) {
                // The wipe tower may be getting smaller. Find the depth for this layer.
                size_t i = 0;
                for (i=0; i<z_and_depth_pairs.size()-1; ++i)
                    if (hh >= z_and_depth_pairs[i].first && hh < z_and_depth_pairs[i+1].first)
                        break;
                d = scale_(z_and_depth_pairs[i].second);
                minCorner = {0.f, -d/2 + scale_(z_and_depth_pairs.front().second/2.f)};
                maxCorner = { minCorner.x() + w, minCorner.y() + d };
            }


            ExtrusionPath path(ExtrusionRole::erWipeTower, 0.0, 0.0, lh);
            path.polyline = { minCorner, {maxCorner.x(), minCorner.y()}, maxCorner, {minCorner.x(), maxCorner.y()}, minCorner };
            paths.push_back({ path });

            // We added the border, now add several parallel lines so we can detect an object that is fully inside the tower.
            // For now, simply use fixed spacing of 3mm.
            for (coord_t y=minCorner.y()+scale_(3.); y<maxCorner.y(); y+=scale_(3.)) {
                path.polyline = { {minCorner.x(), y}, {maxCorner.x(), y} };
                paths.back().emplace_back(path);
            }

            // And of course the stabilization cone and its base...
            if (cone_base_R > 0.) {
                path.polyline.clear();
                double r = cone_base_R * (1 - hh/height);
                for (double alpha=0; alpha<2.01*M_PI; alpha+=2*M_PI/20.)
                    path.polyline.points.emplace_back(Point::new_scale(width/2. + r * std::cos(alpha)/cone_scale_x, depth/2. + r * std::sin(alpha)));
                paths.back().emplace_back(path);
                if (hh == 0.f) { // Cone brim.
                    for (float bw=brim_width; bw>0.f; bw-=3.f) {
                        path.polyline.clear();
                        for (double alpha=0; alpha<2.01*M_PI; alpha+=2*M_PI/20.) // see load_wipe_tower_preview, where the same is a bit clearer
                            path.polyline.points.emplace_back(Point::new_scale(
                                width/2. + cone_base_R * std::cos(alpha)/cone_scale_x * (1. + cone_scale_x*bw/cone_base_R),
                                depth/2. + cone_base_R * std::sin(alpha) * (1. + bw/cone_base_R))
                            );
                        paths.back().emplace_back(path);
                    }
                }
            }

            // Only the first layer has brim.
            if (hh == 0.f) {
                minCorner = minCorner + Point(bd, bd);
                maxCorner = maxCorner - Point(bd, bd);
            }
        }

        // Rotate and translate the tower into the final position.
        for (ExtrusionPaths& ps : paths) {
            for (ExtrusionPath& p : ps) {
                p.polyline.rotate(Geometry::deg2rad(rotation_angle));
                p.polyline.translate(scale_(pos.x()), scale_(pos.y()));
            }
        }

        return paths;
    }
};

struct WipeTowerData
{
    struct WipeTowerMeshData
    {
        Polygon      bottom;
        TriangleMesh real_wipe_tower_mesh;
        TriangleMesh real_brim_mesh;
    };
    // Following section will be consumed by the GCodeGenerator.
    // Tool ordering of a non-sequential print has to be known to calculate the wipe tower.
    // Cache it here, so it does not need to be recalculated during the G-code generation.
    ToolOrdering                                         &tool_ordering;
    // Cache of tool changes per print layer.
    std::unique_ptr<std::vector<WipeTower::ToolChangeResult>> priming;
    std::vector<std::vector<WipeTower::ToolChangeResult>> tool_changes;
    std::unique_ptr<WipeTower::ToolChangeResult>          final_purge;
    std::vector<float>                                    used_filament;
    int                                                   number_of_toolchanges;

    // Depth of the wipe tower to pass to GLCanvas3D for exact bounding box:
    float                                                 depth;
    std::vector<std::pair<float, float>>                  z_and_depth_pairs;
    float                                                 brim_width;
    float                                                 height;
    BoundingBoxf                                          bbx;//including brim
    Vec2f                                                 rib_offset;
    std::optional<WipeTowerMeshData>                      wipe_tower_mesh_data;//added rib_offset
    void clear() {
        priming.reset(nullptr);
        tool_changes.clear();
        final_purge.reset(nullptr);
        used_filament.clear();
        number_of_toolchanges = -1;
        depth = 0.f;
        brim_width = 0.f;
        wipe_tower_mesh_data  = std::nullopt;
    }
    void construct_mesh(float width, float depth, float height, float brim_width, bool is_rib_wipe_tower, float rib_width, float rib_length, bool fillet_wall);

private:
	// Only allow the WipeTowerData to be instantiated internally by Print, 
	// as this WipeTowerData shares reference to Print::m_tool_ordering.
	friend class Print;
	WipeTowerData(ToolOrdering &tool_ordering) : tool_ordering(tool_ordering) { clear(); }
	WipeTowerData(const WipeTowerData & /* rhs */) = delete;
	WipeTowerData &operator=(const WipeTowerData & /* rhs */) = delete;
};

struct PrintStatistics
{
    PrintStatistics() { clear(); }
    std::string                     estimated_normal_print_time;
    std::string                     estimated_silent_print_time;
    double                          total_used_filament;
    double                          total_extruded_volume;
    double                          total_cost;
    int                             total_toolchanges;
    double                          total_weight;
    double                          total_wipe_tower_cost;
    double                          total_wipe_tower_filament;
    unsigned int                    initial_tool;
    std::map<size_t, double>        filament_stats;

    // Config with the filled in print statistics.
    DynamicConfig           config() const;
    // Config with the statistics keys populated with placeholder strings.
    static DynamicConfig    placeholders();
    // Replace the print statistics placeholders in the path.
    std::string             finalize_output_path(const std::string &path_in) const;

    void clear() {
        total_used_filament    = 0.;
        total_extruded_volume  = 0.;
        total_cost             = 0.;
        total_toolchanges      = 0;
        total_weight           = 0.;
        total_wipe_tower_cost  = 0.;
        total_wipe_tower_filament = 0.;
        initial_tool           = 0;
        filament_stats.clear();
    }
    static const std::string FilamentUsedG;
    static const std::string FilamentUsedGMask;
    static const std::string TotalFilamentUsedG;
    static const std::string TotalFilamentUsedGMask;
    static const std::string TotalFilamentUsedGValueMask;
    static const std::string FilamentUsedCm3;
    static const std::string FilamentUsedCm3Mask;
    static const std::string FilamentUsedMm;
    static const std::string FilamentUsedMmMask;
    static const std::string FilamentCost;
    static const std::string FilamentCostMask;
    static const std::string TotalFilamentCost;
    static const std::string TotalFilamentCostMask;
    static const std::string TotalFilamentCostValueMask;
    static const std::string TotalFilamentUsedWipeTower;
    static const std::string TotalFilamentUsedWipeTowerValueMask;
    
};

typedef std::vector<PrintObject*>       PrintObjectPtrs;
typedef std::vector<const PrintObject*> ConstPrintObjectPtrs;
class ConstPrintObjectPtrsAdaptor : public ConstVectorOfPtrsAdaptor<PrintObject> {
    friend Print;
    ConstPrintObjectPtrsAdaptor(const PrintObjectPtrs *data) : ConstVectorOfPtrsAdaptor<PrintObject>(data) {}
};

typedef std::vector<PrintRegion*>       PrintRegionPtrs;
/*
typedef std::vector<const PrintRegion*> ConstPrintRegionPtrs;
class ConstPrintRegionPtrsAdaptor : public ConstVectorOfPtrsAdaptor<PrintRegion> {
    friend Print;
    ConstPrintRegionPtrsAdaptor(const PrintRegionPtrs *data) : ConstVectorOfPtrsAdaptor<PrintRegion>(data) {}
};
*/

struct StatisticsByExtruderCount
{
    // flush weight comes first,then comes filament change time
    FilamentChangeStats stats_by_single_extruder;
    FilamentChangeStats stats_by_multi_extruder_best;
    FilamentChangeStats stats_by_multi_extruder_curr;
    void clear() {
        stats_by_single_extruder.clear();
        stats_by_multi_extruder_best.clear();
        stats_by_multi_extruder_curr.clear();
    }
};

enum FilamentTempType {
    HighTemp=0,
    LowTemp,
    HighLowCompatible,
    Undefine
};

enum FilamentCompatibilityType {
    Compatible,
    HighLowMixed,
    HighMidMixed,
    LowMidMixed
};


struct SpoolmanFilamentConsumptionEstimate
{
    const unsigned int print_config_idx;
    const unsigned int spoolman_spool_id;
    const std::string  filament_name;
    const double       est_used_length;
    const double       est_used_weight;

    SpoolmanFilamentConsumptionEstimate(const unsigned int& print_config_idx, const PrintConfig& config, const double& est_used_length, const double& est_used_weight)
    : print_config_idx(print_config_idx)
    , spoolman_spool_id(config.spoolman_spool_id.get_at(print_config_idx))
    , filament_name(config.filament_settings_id.get_at(print_config_idx))
    , est_used_length(est_used_length)
    , est_used_weight(est_used_weight)
    {}

    // Alternate that allows the estimates to be provided as a std::pair [est_used_length, est_used_weight]
    SpoolmanFilamentConsumptionEstimate(const unsigned int& print_config_idx, const PrintConfig& config, const std::pair<double, double>& estimates) :
        SpoolmanFilamentConsumptionEstimate(print_config_idx, config, estimates.first, estimates.second) {}
};

// The complete print tray with possibly multiple objects.
class Print : public PrintBaseWithState<PrintStep, psCount>
{
private: // Prevents erroneous use by other classes.
    typedef PrintBaseWithState<PrintStep, psCount> Inherited;
    // Bool indicates if supports of PrintObject are top-level contour.
    typedef std::pair<PrintObject *, bool>         PrintObjectInfo;

public:
    Print() = default;
	virtual ~Print() { this->clear(); }

	PrinterTechnology	technology() const noexcept override { return ptFFF; }

    // Methods, which change the state of Print / PrintObject / PrintRegion.
    // The following methods are synchronized with process() and export_gcode(),
    // so that process() and export_gcode() may be called from a background thread.
    // In case the following methods need to modify data processed by process() or export_gcode(),
    // a cancellation callback is executed to stop the background processing before the operation.
    void                clear() override;
    bool                empty() const override { return m_objects.empty(); }
    // List of existing PrintObject IDs, to remove notifications for non-existent IDs.
    std::vector<ObjectID> print_object_ids() const override;

    ApplyStatus         apply(const Model &model, DynamicPrintConfig config) override;

    void                process(long long *time_cost_with_cache = nullptr, bool use_cache = false) override;
    // Exports G-code into a file name based on the path_template, returns the file path of the generated G-code file.
    // If preview_data is not null, the preview_data is filled in for the G-code visualization (not used by the command line Slic3r).
    std::string         export_gcode(const std::string& path_template, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);
    //return 0 means successful
    int                 export_cached_data(const std::string& dir_path, bool with_space=false);
    int                 load_cached_data(const std::string& directory);

    // methods for handling state
    bool                is_step_done(PrintStep step) const { return Inherited::is_step_done(step); }
    // Returns true if an object step is done on all objects and there's at least one object.
    bool                is_step_done(PrintObjectStep step) const;
    // Returns true if the last step was finished with success.
    bool                finished() const override { return this->is_step_done(psGCodeExport); }

    bool                has_infinite_skirt() const;
    bool                has_skirt() const;
    bool                has_brim() const;
    //BBS
    bool                has_auto_brim() const    {
        return std::any_of(m_objects.begin(), m_objects.end(), [](PrintObject* object) { return object->config().brim_type == btAutoBrim; });
    }

    // Returns an empty string if valid, otherwise returns an error message.
    StringObjectException validate(StringObjectException *warning = nullptr, Polygons* collison_polygons = nullptr, std::vector<std::pair<Polygon, float>>* height_polygons = nullptr) const override;
    double              skirt_first_layer_height() const;
    Flow                brim_flow() const;
    Flow                skirt_flow() const;

    std::vector<unsigned int> object_extruders() const;
    std::vector<unsigned int> support_material_extruders() const;
    std::vector<unsigned int> extruders(bool conside_custom_gcode = false) const;
    double              max_allowed_layer_height() const;
    bool                has_support_material() const;
    // Make sure the background processing has no access to this model_object during this call!
    void                auto_assign_extruders(ModelObject* model_object) const;

    const PrintConfig&          config() const { return m_config; }
    const PrintObjectConfig&    default_object_config() const { return m_default_object_config; }
    const PrintRegionConfig& default_region_config() const { return m_default_region_config; }
    ConstPrintObjectPtrsAdaptor objects() const { return ConstPrintObjectPtrsAdaptor(&m_objects); }
    PrintObject*                get_object(size_t idx) { return const_cast<PrintObject*>(m_objects[idx]); }
    const PrintObject*          get_object(size_t idx) const { return m_objects[idx]; }
    // PrintObject by its ObjectID, to be used to uniquely bind slicing warnings to their source PrintObjects
    // in the notification center.
    const PrintObject*          get_object(ObjectID object_id) const {
        auto it = std::find_if(m_objects.begin(), m_objects.end(),
            [object_id](const PrintObject *obj) { return obj->id() == object_id; });
        return (it == m_objects.end()) ? nullptr : *it;
    }
    //BBS: Function to get m_brimMap;
    std::map<ObjectID, ExtrusionEntityCollection>&
        get_brimMap() { return m_brimMap; }

    // How many of PrintObject::copies() over all print objects are there?
    // If zero, then the print is empty and the print shall not be executed.
    unsigned int                num_object_instances() const;

    // For Perl bindings.
    PrintObjectPtrs&            objects_mutable() { return m_objects; }
    PrintRegionPtrs&            print_regions_mutable() { return m_print_regions; }
    std::vector<size_t>         layers_sorted_for_object(float start, float end, std::vector<LayerPtrs> &layers_of_objects, std::vector<BoundingBox> &boundingBox_for_objects, VecOfPoints& objects_instances_shift);
    const ExtrusionEntityCollection& skirt() const { return m_skirt; }
    // Convex hull of the 1st layer extrusions, for bed leveling and placing the initial purge line.
    // It encompasses the object extrusions, support extrusions, skirt, brim, wipe tower.
    // It does NOT encompass user extrusions generated by custom G-code,
    // therefore it does NOT encompass the initial purge line.
    // It does NOT encompass MMU/MMU2 starting (wipe) areas.
    const Polygon&                   first_layer_convex_hull() const { return m_first_layer_convex_hull; }

    const PrintStatistics&      print_statistics() const { return m_print_statistics; }
    PrintStatistics&            print_statistics() { return m_print_statistics; }

    const StatisticsByExtruderCount statistics_by_extruder() const { return m_statistics_by_extruder_count; }
    StatisticsByExtruderCount& statistics_by_extruder() { return m_statistics_by_extruder_count; }

    // Wipe tower support.
    bool                        has_wipe_tower() const;
    const WipeTowerData&        wipe_tower_data(size_t filaments_cnt = 0) const;
    const ToolOrdering& 		tool_ordering() const { return m_tool_ordering; }

    void update_filament_maps_to_config(std::vector<int> f_maps);
    void apply_config_for_render(const DynamicConfig &config);

    // 1 based group ids
    std::vector<int> get_filament_maps() const;
    FilamentMapMode  get_filament_map_mode() const;
    // get the group label of filament
    size_t get_extruder_id(unsigned int filament_id) const;

    const std::vector<std::vector<DynamicPrintConfig>>& get_extruder_filament_info() const { return m_extruder_filament_info; }
    void set_extruder_filament_info(const std::vector<std::vector<DynamicPrintConfig>>& filament_info) { m_extruder_filament_info = filament_info; }

    void set_geometric_unprintable_filaments(const std::vector<std::set<int>> &unprintables_filament_ids) { m_geometric_unprintable_filaments = unprintables_filament_ids; }
    std::vector<std::set<int>> get_geometric_unprintable_filaments() const { return m_geometric_unprintable_filaments;}

    void set_slice_used_filaments(const std::vector<unsigned int> &first_layer_used_filaments, const std::vector<unsigned int> &used_filaments){
        m_slice_used_filaments_first_layer = first_layer_used_filaments;
        m_slice_used_filaments = used_filaments;
    }
    std::vector<unsigned int> get_slice_used_filaments(bool first_layer) const { return first_layer ? m_slice_used_filaments_first_layer : m_slice_used_filaments;}

    /**
    * @brief Determines the unprintable filaments for each extruder based on its physical attributes
    *
    * Currently, the criteria for determining unprintable filament include the following:
    * 1. TPU filaments can only be placed in the master extruder and must be grouped alone.
    * 2. We only support at most 1 tpu filament.
    * 3. An extruder can only accommodate filament with a hardness requirement lower than that of its nozzle.
    *
    * @param used_filaments Totally used filaments when slicing
    * @return A vector of sets representing unprintable filaments for each extruder.Return an empty vecto if extruder num is 1
    */
    std::vector<std::set<int>> get_physical_unprintable_filaments(const std::vector<unsigned int>& used_filaments) const;

    std::vector<double> get_extruder_printable_height() const;
    std::vector<Polygons> get_extruder_printable_polygons() const;
    std::vector<Polygons> get_extruder_unprintable_polygons() const;

    bool                        enable_timelapse_print() const;

	std::string                 output_filename(const std::string &filename_base = std::string()) const override;

	std::string                 get_model_name() const;
	std::string                 get_plate_number_formatted() const;

    size_t                      num_print_regions() const throw() { return m_print_regions.size(); }
    const PrintRegion&          get_print_region(size_t idx) const  { return *m_print_regions[idx]; }
    const ToolOrdering&         get_tool_ordering() const { return m_wipe_tower_data.tool_ordering; }

    //BBS: plate's origin related functions
    void set_plate_origin(Vec3d origin) { m_origin = origin; }
    const Vec3d get_plate_origin() const { return m_origin; }
    //BBS: export gcode from previous gcode file from 3mf
    void set_gcode_file_ready();
    void set_gcode_file_invalidated();
    void export_gcode_from_previous_file(const std::string& file, GCodeProcessorResult* result, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);
    //BBS: add modify_count logic
    int get_modified_count() const {return m_modified_count;}
    //BBS: add status for whether support used
    bool is_support_used() const {return m_support_used;}
    std::string get_conflict_string() const
    {
        std::string result;
        if (m_conflict_result) {
            result = "Found gcode path conflicts between object " + m_conflict_result.value()._objName1 + " and " + m_conflict_result.value()._objName2;
        }

        return result;
    }

    //BBS
    static StringObjectException sequential_print_clearance_valid(const Print &print, Polygons *polygons = nullptr, std::vector<std::pair<Polygon, float>>* height_polygons = nullptr);
    ConflictResultOpt            get_conflict_result() const { return m_conflict_result; }

    // Return 4 wipe tower corners in the world coordinates (shifted and rotated), including the wipe tower brim.
    Points first_layer_wipe_tower_corners(bool check_wipe_tower_existance=true) const;

    //SoftFever
    bool &is_BBL_printer() { return m_isBBLPrinter; }
    const bool is_BBL_printer() const { return m_isBBLPrinter; }
    bool &is_QIDI_printer() { return m_isQIDIPrinter; }
    const bool is_QIDI_printer() const { return m_isQIDIPrinter; }
    CalibMode& calib_mode() { return m_calib_params.mode; }
    const CalibMode calib_mode() const { return m_calib_params.mode; }
    void set_calib_params(const Calib_Params& params);
    const Calib_Params& calib_params() const { return m_calib_params; }
    Vec2d translate_to_print_space(const Vec2d &point) const;
    float               get_wipe_tower_depth() const { return m_wipe_tower_data.depth; }
    BoundingBoxf        get_wipe_tower_bbx() const { return m_wipe_tower_data.bbx; }
    Vec2f               get_rib_offset() const { return m_wipe_tower_data.rib_offset; }
    const FakeWipeTower& get_fake_wipe_tower() const { return m_fake_wipe_tower; }

    void set_check_multi_filaments_compatibility(bool check) { m_need_check_multi_filaments_compatibility = check; }
    bool need_check_multi_filaments_compatibility() const { return m_need_check_multi_filaments_compatibility; }

    // scaled point
    Vec2d translate_to_print_space(const Point &point) const;
    static FilamentTempType get_filament_temp_type(const std::string& filament_type);
    static int get_hrc_by_nozzle_type(const NozzleType& type);
    static std::vector<std::string> get_incompatible_filaments_by_nozzle(const float nozzle_diameter, const std::optional<NozzleVolumeType> nozzle_volume_type = std::nullopt);
    static FilamentCompatibilityType check_multi_filaments_compatibility(const std::vector<std::string>& filament_types);
    // similar to check_multi_filaments_compatibility, but the input is int, and may be negative (means unset)
    static bool is_filaments_compatible(const std::vector<int>& types);
    // get the compatible filament type of a multi-material object
    // Rule:
    // 1. LowTemp+HighLowCompatible=LowTemp
    // 2. HighTemp++HighLowCompatible=HighTemp
    // 3. LowTemp+HighTemp+...=HighLowCompatible
    // Unset types are just ignored.
    static int get_compatible_filament_type(const std::set<int>& types);

    bool is_all_objects_are_short() const {
        return std::all_of(this->objects().begin(), this->objects().end(), [&](PrintObject* obj) { return obj->height() < scale_(this->config().nozzle_height.value); });
    }
    
    // Orca: Implement prusa's filament shrink compensation approach
    // Returns if all used filaments have same shrinkage compensations.
     bool has_same_shrinkage_compensations() const;
    // Returns scaling for each axis representing shrinkage compensations in each axis.
     Vec3d shrinkage_compensation() const;

    std::tuple<float, float> object_skirt_offset(double margin_height = 0) const;

    std::vector<SpoolmanFilamentConsumptionEstimate> get_spoolman_filament_consumption_estimates() const;

protected:
    // Invalidates the step, and its depending steps in Print.
    bool                invalidate_step(PrintStep step);

private:
    //BBS
    static StringObjectException check_multi_filament_valid(const Print &print);

    bool                has_tpu_filament() const;
    bool                invalidate_state_by_config_options(const ConfigOptionResolver &new_config, const std::vector<t_config_option_key> &opt_keys);

    void                _make_skirt();
    void                _make_wipe_tower();
    void                finalize_first_layer_convex_hull();

    // Islands of objects and their supports extruded at the 1st layer.
    Polygons            first_layer_islands() const;

    PrintConfig                             m_config;
    PrintObjectConfig                       m_default_object_config;
    PrintRegionConfig                       m_default_region_config;
    PrintObjectPtrs                         m_objects;
    PrintRegionPtrs                         m_print_regions;
    
    //SoftFever
    bool m_isBBLPrinter;
    bool m_isQIDIPrinter;

    // Ordered collections of extrusion paths to build skirt loops and brim.
    ExtrusionEntityCollection               m_skirt;
    // BBS: collecting extrusion paths to build brim by objs
    std::map<ObjectID, ExtrusionEntityCollection>         m_brimMap;
    std::map<ObjectID, ExtrusionEntityCollection>         m_supportBrimMap;
    // Convex hull of the 1st layer extrusions.
    // It encompasses the object extrusions, support extrusions, skirt, brim, wipe tower.
    // It does NOT encompass user extrusions generated by custom G-code,
    // therefore it does NOT encompass the initial purge line.
    // It does NOT encompass MMU/MMU2 starting (wipe) areas.
    Polygon                                 m_first_layer_convex_hull;
    Points                                  m_skirt_convex_hull;

    std::vector<std::vector<DynamicPrintConfig>> m_extruder_filament_info;

    // Following section will be consumed by the GCodeGenerator.
    ToolOrdering 							m_tool_ordering;
    WipeTowerData                           m_wipe_tower_data {m_tool_ordering};

    // Estimated print time, filament consumed.
    PrintStatistics                         m_print_statistics;
    bool                                    m_support_used {false};
    StatisticsByExtruderCount               m_statistics_by_extruder_count;

    std::vector<unsigned int> m_slice_used_filaments;
    std::vector<unsigned int> m_slice_used_filaments_first_layer;

    //BBS: plate's origin
    Vec3d   m_origin;
    //BBS: modified_count
    int     m_modified_count {0};
    //BBS
    ConflictResultOpt m_conflict_result;
    FakeWipeTower     m_fake_wipe_tower;
    bool              m_has_auto_filament_map_result{false};

    std::vector<std::set<int>> m_geometric_unprintable_filaments;

    //SoftFever: calibration
    Calib_Params m_calib_params;

    bool m_need_check_multi_filaments_compatibility{true};

    // To allow GCode to set the Print's GCodeExport step status.
    friend class GCode;
    // Allow PrintObject to access m_mutex and m_cancel_callback.
    friend class PrintObject;

public:
    //BBS: this was a print config and now seems to be useless so we move it to here
    // ORCA: parameter below is now back to being a user option (min_skirt_length)
    //static float min_skirt_length;
};


} /* slic3r_Print_hpp_ */

#endif
