// Based on implementation by @platsch

#ifndef slic3r_SlicingAdaptive_hpp_
#define slic3r_SlicingAdaptive_hpp_

#include "Slicing.hpp"
#include "admesh/stl.h"

namespace Slic3r
{

class ModelVolume;

class SlicingAdaptive
{
public:
    void  clear();
    void  set_slicing_parameters(SlicingParameters params) { m_slicing_params = params; }
    void  prepare(const ModelObject &object);
    // Return next layer height starting from the last print_z, using a quality measure
    // (quality in range from 0 to 1, 0 - highest quality at low layer heights, 1 - lowest print quality at high layer heights).
    // The layer height curve shall be centered roughly around the default profile's layer height for quality 0.5.
	float next_layer_height(const float print_z, float quality, size_t &current_facet);
    float horizontal_facet_distance(float z);

	struct FaceZ {
		std::pair<float, float> z_span;
		// Cosine of the normal vector towards the Z axis.
		float					n_cos;
		// Sine of the normal vector towards the Z axis.
		float					n_sin;
	};

protected:
	SlicingParameters 		m_slicing_params;

	std::vector<FaceZ>		m_faces;
};

}; // namespace Slic3r

#endif /* slic3r_SlicingAdaptive_hpp_ */
