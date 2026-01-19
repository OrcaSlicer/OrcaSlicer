#ifndef slic3r_SupportLayer_hpp_
#define slic3r_SupportLayer_hpp_

#include <oneapi/tbb/scalable_allocator.h>
#include <oneapi/tbb/spin_mutex.h>
// for Slic3r::deque
#include "../libslic3r.h"
#include "../ClipperUtils.hpp"
#include "../Polygon.hpp"

namespace Slic3r {

// Support layer type to be used by SupportGeneratorLayer. This type carries a much more detailed information
// about the support layer type than the final support layers stored in a PrintObject.
enum class SupporLayerType {
	Unknown = 0,
	// Ratft base layer, to be printed with the support material.
	RaftBase,
	// Raft interface layer, to be printed with the support interface material. 
	RaftInterface,
	// Bottom contact layer placed over a top surface of an object. To be printed with a support interface material.
	BottomContact,
	// Dense interface layer, to be printed with the support interface material.
	// This layer is separated from an object by an BottomContact layer.
	BottomInterface,
	// Sparse base support layer, to be printed with a support material.
	Base,
	// Dense interface layer, to be printed with the support interface material.
	// This layer is separated from an object with TopContact layer.
	TopInterface,
	// Top contact layer directly supporting an overhang. To be printed with a support interface material.
	TopContact,
	// Some undecided type yet. It will turn into Base first, then it may turn into BottomInterface or TopInterface.
	Intermediate,
};

// A support layer type used internally by the SupportMaterial class. This class carries a much more detailed
// information about the support layer than the layers stored in the PrintObject, mainly
// the SupportGeneratorLayer is aware of the bridging flow and the interface gaps between the object and the support.
class SupportGeneratorLayer
{
public:
	void reset() {
		*this = SupportGeneratorLayer();
	}

	bool operator==(const SupportGeneratorLayer &layer2) const {
		return print_z == layer2.print_z && height == layer2.height && bridging == layer2.bridging;
	}

	// Order the layers by lexicographically by an increasing print_z and a decreasing layer height.
	bool operator<(const SupportGeneratorLayer &layer2) const {
		if (print_z < layer2.print_z) {
			return true;
		} else if (print_z == layer2.print_z) {
		 	if (height > layer2.height)
		 		return true;
		 	else if (height == layer2.height) {
		 		// Bridging layers first.
		 	 	return bridging && ! layer2.bridging;
		 	} else
		 		return false;
		} else
			return false;
	}

	void merge(SupportGeneratorLayer &&rhs) {
        // The union_() does not support move semantic yet, but maybe one day it will.
        this->polygons = union_(this->polygons, std::move(rhs.polygons));
        auto merge = [](std::unique_ptr<Polygons> &dst, std::unique_ptr<Polygons> &src) {
        	if (! dst || dst->empty())
        		dst = std::move(src);
        	else if (src && ! src->empty())
    			*dst = union_(*dst, std::move(*src));
        };
        merge(this->contact_polygons,  rhs.contact_polygons);
        merge(this->overhang_polygons, rhs.overhang_polygons);
        merge(this->enforcer_polygons, rhs.enforcer_polygons);
        rhs.reset();
    }

	// For the bridging flow, bottom_print_z will be above bottom_z to account for the vertical separation.
	// For the non-bridging flow, bottom_print_z will be equal to bottom_z.
	coordf_t bottom_print_z() const { return print_z - height; }

	// To sort the extremes of top / bottom interface layers.
	coordf_t extreme_z() const { return (this->layer_type == SupporLayerType::TopContact) ? this->bottom_z : this->print_z; }

	SupporLayerType layer_type { SupporLayerType::Unknown };
	// Z used for printing, in unscaled coordinates.
	coordf_t print_z { 0 };
	// Bottom Z of this layer. For soluble layers, bottom_z + height = print_z,
	// otherwise bottom_z + gap + height = print_z.
	coordf_t bottom_z { 0 };
	// Layer height in unscaled coordinates.
	coordf_t height { 0 };
	// Index of a PrintObject layer_id supported by this layer. This will be set for top contact layers.
	// If this is not a contact layer, it will be set to size_t(-1).
	size_t 	 idx_object_layer_above { size_t(-1) };
	// Index of a PrintObject layer_id, which supports this layer. This will be set for bottom contact layers.
	// If this is not a contact layer, it will be set to size_t(-1).
	size_t 	 idx_object_layer_below { size_t(-1) };
	// Use a bridging flow when printing this support layer.
	bool 	 bridging { false };

	// Polygons to be filled by the support pattern.
	Polygons polygons;
	// Currently for the contact layers only.
	std::unique_ptr<Polygons> contact_polygons;
	std::unique_ptr<Polygons> overhang_polygons;
	// Enforcers need to be propagated independently in case the "support on build plate only" option is enabled.
	std::unique_ptr<Polygons> enforcer_polygons;
};

// Layers are allocated and owned by a deque. Once a layer is allocated, it is maintained
// up to the end of a generate() method. The layer storage may be replaced by an allocator class in the future, 
// which would allocate layers by multiple chunks.
class SupportGeneratorLayerStorage {
public:
	SupportGeneratorLayer& allocate_unguarded(SupporLayerType layer_type) { 
		m_storage.emplace_back();
		m_storage.back().layer_type = layer_type;
	    return m_storage.back();
	}

	SupportGeneratorLayer& allocate(SupporLayerType layer_type)
	{ 
		m_mutex.lock();
		m_storage.emplace_back();
	    SupportGeneratorLayer *layer_new = &m_storage.back();
		m_mutex.unlock();
	    layer_new->layer_type = layer_type;
	    return *layer_new;
	}

private:
	template<typename BaseType>
	using Allocator = tbb::scalable_allocator<BaseType>;
	Slic3r::deque<SupportGeneratorLayer, Allocator<SupportGeneratorLayer>> 		m_storage;
	tbb::spin_mutex                         									m_mutex;
};
using SupportGeneratorLayersPtr		= std::vector<SupportGeneratorLayer*>;

} // namespace Slic3r

#endif /* slic3r_SupportLayer_hpp_ */
