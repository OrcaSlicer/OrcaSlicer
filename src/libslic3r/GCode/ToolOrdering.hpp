// Ordering of the tools to minimize tool switches.

#ifndef slic3r_ToolOrdering_hpp_
#define slic3r_ToolOrdering_hpp_

#include "../libslic3r.h"

#include <utility>

#include <boost/container/small_vector.hpp>
#include "../FilamentGroup.hpp"
#include "../ExtrusionEntity.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

class Print;
class PrintObject;
class LayerTools;
namespace CustomGCode { struct Item; }
class PrintRegion;

// Object of this class holds information about whether an extrusion is printed immediately
// after a toolchange (as part of infill/perimeter wiping) or not. One extrusion can be a part
// of several copies - this has to be taken into account.
class WipingExtrusions
{
public:
    bool is_anything_overridden() const {   // if there are no overrides, all the agenda can be skipped - this function can tell us if that's the case
        return something_overridden;
    }

    // When allocating extruder overrides of an object's ExtrusionEntity, overrides for maximum 3 copies are allocated in place.
    typedef boost::container::small_vector<int32_t, 3> ExtruderPerCopy;

    // This is called from GCode::process_layer - see implementation for further comments:
    const ExtruderPerCopy* get_extruder_overrides(const ExtrusionEntity* entity, const PrintObject* object, int correct_extruder_id, size_t num_of_copies);
    int get_support_extruder_overrides(const PrintObject* object);
    int get_support_interface_extruder_overrides(const PrintObject* object);

    // This function goes through all infill entities, decides which ones will be used for wiping and
    // marks them by the extruder id. Returns volume that remains to be wiped on the wipe tower:
    float mark_wiping_extrusions(const Print& print, unsigned int old_extruder, unsigned int new_extruder, float volume_to_wipe);

    void ensure_perimeters_infills_order(const Print& print);

    bool is_overriddable(const ExtrusionEntityCollection& ee, const PrintConfig& print_config, const PrintObject& object, const PrintRegion& region) const;
    bool is_overriddable_and_mark(const ExtrusionEntityCollection& ee, const PrintConfig& print_config, const PrintObject& object, const PrintRegion& region) {
    	bool out = this->is_overriddable(ee, print_config, object, region);
    	this->something_overridable |= out;
    	return out;
    }

    // BBS
    bool is_support_overriddable(const ExtrusionRole role, const PrintObject& object) const;
    bool is_support_overriddable_and_mark(const ExtrusionRole role, const PrintObject& object) {
        bool out = this->is_support_overriddable(role, object);
        this->something_overridable |= out;
        return out;
    }

    bool is_support_overridden(const PrintObject* object) const {
        return support_map.find(object) != support_map.end();
    }

    bool is_support_interface_overridden(const PrintObject* object) const {
        return support_intf_map.find(object) != support_intf_map.end();
    }

    void set_layer_tools_ptr(const LayerTools* lt) { m_layer_tools = lt; }

private:
    int first_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const;
    int last_nonsoluble_extruder_on_layer(const PrintConfig& print_config) const;

    // This function is called from mark_wiping_extrusions and sets extruder that it should be printed with (-1 .. as usual)
    void set_extruder_override(const ExtrusionEntity* entity, const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies);
    // BBS
    void set_support_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies);
    void set_support_interface_extruder_override(const PrintObject* object, size_t copy_id, int extruder, size_t num_of_copies);

    // Returns true in case that entity is not printed with its usual extruder for a given copy:
    bool is_entity_overridden(const ExtrusionEntity* entity, const PrintObject *object, size_t copy_id) const {
        auto it = entity_map.find(std::make_tuple(entity, object));
        return it == entity_map.end() ? false : it->second[copy_id] != -1;
    }

    std::map<std::tuple<const ExtrusionEntity*, const PrintObject *>, ExtruderPerCopy> entity_map;  // to keep track of who prints what
    // BBS
    std::map<const PrintObject*, int> support_map;
    std::map<const PrintObject*, int> support_intf_map;
    bool something_overridable = false;
    bool something_overridden = false;
    const LayerTools* m_layer_tools = nullptr;    // so we know which LayerTools object this belongs to
};


struct FilamentChangeStats
{
    int filament_flush_weight{0};
    int filament_change_count{0};
    int extruder_change_count{0};

    void clear(){
        filament_flush_weight = 0;
        filament_change_count = 0;
        extruder_change_count = 0;
    }

    FilamentChangeStats& operator+=(const FilamentChangeStats& other) {
        this->filament_flush_weight += other.filament_flush_weight;
        this->filament_change_count += other.filament_change_count;
        this->extruder_change_count += other.extruder_change_count;
        return *this;
    }

    FilamentChangeStats operator+(const FilamentChangeStats& other){
        FilamentChangeStats ret;
        ret.filament_flush_weight = this->filament_flush_weight + other.filament_flush_weight;
        ret.filament_change_count = this->filament_change_count + other.filament_change_count;
        ret.extruder_change_count = this->extruder_change_count + other.extruder_change_count;
        return ret;
    }

};


class LayerTools
{
public:
    LayerTools(const coordf_t z) : print_z(z) {}

    // Changing these operators to epsilon version can make a problem in cases where support and object layers get close to each other.
    // In case someone tries to do it, make sure you know what you're doing and test it properly (slice multiple objects at once with supports).
    bool operator< (const LayerTools &rhs) const { return print_z < rhs.print_z; }
    bool operator==(const LayerTools &rhs) const { return print_z == rhs.print_z; }

    bool is_extruder_order(unsigned int a, unsigned int b) const;
    bool has_extruder(unsigned int extruder) const { return std::find(this->extruders.begin(), this->extruders.end(), extruder) != this->extruders.end(); }

    // Return a zero based extruder from the region, or extruder_override if overriden.
    unsigned int wall_filament(const PrintRegion &region) const;
    unsigned int sparse_infill_filament(const PrintRegion &region) const;
    unsigned int solid_infill_filament(const PrintRegion &region) const;
	// Returns a zero based extruder this eec should be printed with, according to PrintRegion config or extruder_override if overriden.
	unsigned int extruder(const ExtrusionEntityCollection &extrusions, const PrintRegion &region) const;

    coordf_t 					print_z	= 0.;
    bool 						has_object = false;
    bool						has_support = false;
    // Zero based extruder IDs, ordered to minimize tool switches.
    std::vector<unsigned int> 	extruders;
    // If per layer extruder switches are inserted by the G-code preview slider, this value contains the new (1 based) extruder, with which the whole object layer is being printed with.
    // If not overriden, it is set to 0.
    unsigned int 				extruder_override = 0;
    // Should a skirt be printed at this layer?
    // Layers are marked for infinite skirt aka draft shield. Not all the layers have to be printed.
    bool                        has_skirt = false;
    // Will there be anything extruded on this layer for the wipe tower?
    // Due to the support layers possibly interleaving the object layers,
    // wipe tower will be disabled for some support only layers.
    bool 						has_wipe_tower = false;
    // Number of wipe tower partitions to support the required number of tool switches
    // and to support the wipe tower partitions above this one.
    size_t                      wipe_tower_partitions = 0;
    coordf_t 					wipe_tower_layer_height = 0.;
    // Custom G-code (color change, extruder switch, pause) to be performed before this layer starts to print.
    const CustomGCode::Item    *custom_gcode = nullptr;

    WipingExtrusions& wiping_extrusions() {
        m_wiping_extrusions.set_layer_tools_ptr(this);
        return m_wiping_extrusions;
    }

private:
    // This object holds list of extrusion that will be used for extruder wiping
    WipingExtrusions m_wiping_extrusions;
};

class ToolOrdering
{
public:
    enum FilamentChangeMode {
        SingleExt,
        MultiExtBest,
        MultiExtCurr
    };
    ToolOrdering() = default;

    // For the use case when each object is printed separately
    // (print->config().print_sequence == PrintSequence::ByObject is true).
    ToolOrdering(const PrintObject &object, unsigned int first_extruder, bool prime_multi_material = false);

    // For the use case when all objects are printed at once.
    // (print->config().print_sequence == PrintSequence::ByObject is false).
    ToolOrdering(const Print& print, unsigned int first_extruder, bool prime_multi_material = false);

    void handle_dontcare_extruder(const std::vector<unsigned int>& first_layer_tool_order);
    void handle_dontcare_extruder(unsigned int first_extruder);

    void sort_and_build_data(const PrintObject &object, unsigned int first_extruder, bool prime_multi_material = false);
    void sort_and_build_data(const Print& print, unsigned int first_extruder, bool prime_multi_material = false);

    void    clear() {
        m_layer_tools.clear();
        m_stats_by_single_extruder.clear();
        m_stats_by_multi_extruder_best.clear();
        m_stats_by_multi_extruder_curr.clear();
    }

    // Only valid for non-sequential print:
	// Assign a pointer to a custom G-code to the respective ToolOrdering::LayerTools.
	// Ignore color changes, which are performed on a layer and for such an extruder, that the extruder will not be printing above that layer.
	// If multiple events are planned over a span of a single layer, use the last one.
	void 				assign_custom_gcodes(const Print &print);

    // Get the first extruder printing, including the extruder priming areas, returns -1 if there is no layer printed.
    unsigned int   		first_extruder() const { return m_first_printing_extruder; }

    // Get the first extruder printing the layer_tools, returns -1 if there is no layer printed.
    unsigned int   		last_extruder() const { return m_last_printing_extruder; }

    // For a multi-material print, the printing extruders are ordered in the order they shall be primed.
    const std::vector<unsigned int>& all_extruders() const { return m_all_printing_extruders; }

    // Find LayerTools with the closest print_z.
    const LayerTools&	tools_for_layer(coordf_t print_z) const;
    LayerTools&			tools_for_layer(coordf_t print_z) { return const_cast<LayerTools&>(std::as_const(*this).tools_for_layer(print_z)); }

    const LayerTools&   front()       const { return m_layer_tools.front(); }
    const LayerTools&   back()        const { return m_layer_tools.back(); }
    std::vector<LayerTools>::const_iterator begin() const { return m_layer_tools.begin(); }
    std::vector<LayerTools>::const_iterator end()   const { return m_layer_tools.end(); }
    bool 				empty()       const { return m_layer_tools.empty(); }
    std::vector<LayerTools>& layer_tools() { return m_layer_tools; }
    bool 				has_wipe_tower() const { return ! m_layer_tools.empty() && m_first_printing_extruder != (unsigned int)-1 && m_layer_tools.front().has_wipe_tower; }

    int                 get_most_used_extruder() const { return most_used_extruder; }
    /*
    * called in single extruder mode, the value in map are all 0
    * called in dual extruder mode, the value in map will be 0 or 1
    * 0 based group id
    */
    static std::vector<int> get_recommended_filament_maps(const std::vector<std::vector<unsigned int>>& layer_filaments, const Print* print,const FilamentMapMode mode, const std::vector<std::set<int>>& physical_unprintables, const std::vector<std::set<int>>& geometric_unprintables);

    // should be called after doing reorder
    FilamentChangeStats get_filament_change_stats(FilamentChangeMode mode);
    void                cal_most_used_extruder(const PrintConfig &config);
    float               cal_max_additional_fan(const PrintConfig &config);
    bool                cal_non_support_filaments(const PrintConfig &config,
                                                  unsigned int &     first_non_support_filament,
                                                  std::vector<int> & initial_non_support_filaments,
                                                  std::vector<int> & initial_filaments);

    bool                has_non_support_filament(const PrintConfig &config);

private:
    void				initialize_layers(std::vector<coordf_t> &zs);
    void 				collect_extruders(const PrintObject &object, const std::vector<std::pair<double, unsigned int>> &per_layer_extruder_switches);
    void 				fill_wipe_tower_partitions(const PrintConfig &config, coordf_t object_bottom_z, coordf_t max_layer_height);
    void                mark_skirt_layers(const PrintConfig &config, coordf_t max_layer_height);
    void 				collect_extruder_statistics(bool prime_multi_material);
    void                reorder_extruders_for_minimum_flush_volume(bool reorder_first_layer);

    // BBS
    std::vector<unsigned int> generate_first_layer_tool_order(const Print& print);
    std::vector<unsigned int> generate_first_layer_tool_order(const PrintObject& object);

    std::vector<LayerTools>    m_layer_tools;
    // First printing extruder, including the multi-material priming sequence.
    unsigned int               m_first_printing_extruder = (unsigned int)-1;
    // Final printing extruder.
    unsigned int               m_last_printing_extruder  = (unsigned int)-1;
    // All extruders, which extrude some material over m_layer_tools.
    std::vector<unsigned int>  m_all_printing_extruders;
    const DynamicPrintConfig*  m_print_full_config = nullptr;
    const PrintConfig*         m_print_config_ptr = nullptr;
    const PrintObject*         m_print_object_ptr = nullptr;
    Print*                     m_print;
    bool                       m_sorted = false;
    bool                       m_is_BBL_printer = false;

    FilamentChangeStats        m_stats_by_single_extruder;
    FilamentChangeStats        m_stats_by_multi_extruder_curr;
    FilamentChangeStats        m_stats_by_multi_extruder_best;

    int                        most_used_extruder;
};

} // namespace SLic3r

#endif /* slic3r_ToolOrdering_hpp_ */
