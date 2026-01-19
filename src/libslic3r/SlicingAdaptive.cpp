#include "libslic3r.h"
#include "Model.hpp"
#include "TriangleMesh.hpp"
#include "SlicingAdaptive.hpp"

#include <boost/log/trivial.hpp>
#include <cfloat>

// Based on the work of Florens Waserfall (@platch on github)
// and his paper
// Florens Wasserfall, Norman Hendrich, Jianwei Zhang:
// Adaptive Slicing for the FDM Process Revisited
// 13th IEEE Conference on Automation Science and Engineering (CASE-2017), August 20-23, Xi'an, China. DOI: 10.1109/COASE.2017.8256074
// https://tams.informatik.uni-hamburg.de/publications/2017/Adaptive%20Slicing%20for%20the%20FDM%20Process%20Revisited.pdf

// Vojtech believes that there is a bug in @platch's derivation of the triangle area error metric.
// Following Octave code paints graphs of recommended layer height versus surface slope angle.
#if 0
adeg=0:1:85;
a=adeg*pi/180;
t=tan(a);
tsqr=sqrt(tan(a));
lerr=1./cos(a);
lerr2=1./(0.3+cos(a));
plot(adeg, t, 'b', adeg, sqrt(t), 'g', adeg, 0.5 * lerr, 'm', adeg, 0.5 * lerr2, 'r')
xlabel("angle(deg), 0 - horizontal wall, 90 - vertical wall");
ylabel("layer height");
legend("tan(a) as cura - topographic lines distance limit", "sqrt(tan(a)) as PrusaSlicer - error triangle area limit", "old slic3r - max distance metric", "new slic3r - Waserfall paper");
#endif

#ifndef NDEBUG
	#define ADAPTIVE_LAYER_HEIGHT_DEBUG
#endif /* NDEBUG */

namespace Slic3r
{

// By Florens Waserfall aka @platch:
// This constant essentially describes the volumetric error at the surface which is induced 
// by stacking "elliptic" extrusion threads. It is empirically determined by
// 1. measuring the surface profile of printed parts to find
// the ratio between layer height and profile height and then
// 2. computing the geometric difference between the model-surface and the elliptic profile.
//
// The definition of the roughness formula is in 
// https://tams.informatik.uni-hamburg.de/publications/2017/Adaptive%20Slicing%20for%20the%20FDM%20Process%20Revisited.pdf
// (page 51, formula (8))
// Currenty @platch's error metric formula is not used.
//static constexpr const double SURFACE_CONST = 0.18403;

// for a given facet, compute maximum height within the allowed surface roughness / stairstepping deviation
static inline float layer_height_from_slope(const SlicingAdaptive::FaceZ &face, float max_surface_deviation)
{
// @platch's formula, see his paper "Adaptive Slicing for the FDM Process Revisited".
//    return float(max_surface_deviation / (SURFACE_CONST + 0.5 * std::abs(normal_z)));
	
// Constant stepping in horizontal direction, as used by Cura.
//    return (face.n_cos > 1e-5) ? float(max_surface_deviation * face.n_sin / face.n_cos) : FLT_MAX;

// Constant error measured as an area of the surface error triangle, Vojtech's formula.
//    return (face.n_cos > 1e-5) ? float(1.44 * max_surface_deviation * sqrt(face.n_sin / face.n_cos)) : FLT_MAX;

// Constant error measured as an area of the surface error triangle, Vojtech's formula with clamping to roughness at 90 degrees.
    return std::min(max_surface_deviation / 0.184f, (face.n_cos > 1e-5) ? float(1.44 * max_surface_deviation * sqrt(face.n_sin / face.n_cos)) : FLT_MAX);

// Constant stepping along the surface, equivalent to the "surface roughness" metric by Perez and later Pandey et all, see @platch's paper for references.
//    return float(max_surface_deviation * face.n_sin);
}

void SlicingAdaptive::clear()
{
	m_faces.clear();
}

void SlicingAdaptive::prepare(const ModelObject &object)
{
    this->clear();

    TriangleMesh		 mesh			= object.raw_mesh();
    const ModelInstance &first_instance = *object.instances.front();
    mesh.transform(first_instance.get_matrix(), first_instance.is_left_handed());

    // 1) Collect faces from mesh.
    m_faces.reserve(mesh.facets_count());
	for (stl_triangle_vertex_indices face : mesh.its.indices) {
		stl_vertex vertex[3] = { mesh.its.vertices[face[0]], mesh.its.vertices[face[1]], mesh.its.vertices[face[2]] };
		stl_vertex n         = face_normal_normalized(vertex);
		std::pair<float, float> face_z_span {
			std::min(std::min(vertex[0].z(), vertex[1].z()), vertex[2].z()),
			std::max(std::max(vertex[0].z(), vertex[1].z()), vertex[2].z())
		};
		m_faces.emplace_back(FaceZ({ face_z_span, std::abs(n.z()), std::sqrt(n.x() * n.x() + n.y() * n.y()) }));
    }

	// 2) Sort faces lexicographically by their Z span.
	std::sort(m_faces.begin(), m_faces.end(), [](const FaceZ &f1, const FaceZ &f2) { return f1.z_span < f2.z_span; });
}

// current_facet is in/out parameter, rememebers the index of the last face of m_faces visited, 
// where this function will start from.
// print_z - the top print surface of the previous layer.
// returns height of the next layer.
float SlicingAdaptive::next_layer_height(const float print_z, float quality_factor, size_t &current_facet)
{
	float  height = (float)m_slicing_params.max_layer_height;

	float  max_surface_deviation;

	{
#if 0
// @platch's formula for quality:
	    double delta_min = SURFACE_CONST * m_slicing_params.min_layer_height;
	    double delta_mid = (SURFACE_CONST + 0.5) * m_slicing_params.layer_height;
	    double delta_max = (SURFACE_CONST + 0.5) * m_slicing_params.max_layer_height;
#else
// Vojtech's formula for triangle area error metric.
	    double delta_min = m_slicing_params.min_layer_height;
	    double delta_mid = m_slicing_params.layer_height;
	    double delta_max = m_slicing_params.max_layer_height;
#endif
	    max_surface_deviation = (quality_factor < 0.5f) ?
	    	lerp(delta_min, delta_mid, 2. * quality_factor) :
	    	lerp(delta_max, delta_mid, 2. * (1. - quality_factor));
	}
	
	// find all facets intersecting the slice-layer
	size_t ordered_id = current_facet;
	{
		bool first_hit = false;
		for (; ordered_id < m_faces.size(); ++ ordered_id) {
	        const std::pair<float, float> &zspan = m_faces[ordered_id].z_span;
	        // facet's minimum is higher than slice_z -> end loop
			if (zspan.first >= print_z)
				break;
			// facet's maximum is higher than slice_z -> store the first event for next cusp_height call to begin at this point
			if (zspan.second > print_z) {
				// first event?
				if (! first_hit) {
					first_hit = true;
					current_facet = ordered_id;
	            }
				// skip touching facets which could otherwise cause small cusp values
				if (zspan.second < print_z + EPSILON)
					continue;
				// compute cusp-height for this facet and store minimum of all heights
				height = std::min(height, layer_height_from_slope(m_faces[ordered_id], max_surface_deviation));
	        }
		}
	}

	// lower height limit due to printer capabilities
	height = std::max(height, float(m_slicing_params.min_layer_height));

	// check for sloped facets inside the determined layer and correct height if necessary
	if (height > float(m_slicing_params.min_layer_height)) {
		for (; ordered_id < m_faces.size(); ++ ordered_id) {
            const std::pair<float, float> &zspan = m_faces[ordered_id].z_span;
            // facet's minimum is higher than slice_z + height -> end loop
			if (zspan.first >= print_z + height)
				break;

			// skip touching facets which could otherwise cause small cusp values
			if (zspan.second < print_z + EPSILON)
				continue;

			// Compute cusp-height for this facet and check against height.
            float reduced_height = layer_height_from_slope(m_faces[ordered_id], max_surface_deviation);

			float z_diff = zspan.first - print_z;
			if (reduced_height < z_diff) {
				assert(z_diff < height + EPSILON);
				// The currently visited triangle's slope limits the next layer height so much, that
				// the lowest point of the currently visible triangle is already above the newly proposed layer height.
				// This means, that we need to limit the layer height so that the offending newly visited triangle
				// is just above of the new layer.
#ifdef ADAPTIVE_LAYER_HEIGHT_DEBUG
                BOOST_LOG_TRIVIAL(trace) << "cusp computation, height is reduced from " << height << "to " << z_diff << " due to z-diff";
#endif /* ADAPTIVE_LAYER_HEIGHT_DEBUG */
				height = z_diff;
			} else if (reduced_height < height) {
#ifdef ADAPTIVE_LAYER_HEIGHT_DEBUG
				BOOST_LOG_TRIVIAL(trace) << "adaptive layer computation: height is reduced from " << height << "to " << reduced_height << " due to higher facet";
#endif /* ADAPTIVE_LAYER_HEIGHT_DEBUG */
				height = reduced_height;
			}
		}
		// lower height limit due to printer capabilities again
		height = std::max(height, float(m_slicing_params.min_layer_height));
	}

#ifdef ADAPTIVE_LAYER_HEIGHT_DEBUG
    BOOST_LOG_TRIVIAL(trace) << "adaptive layer computation, layer-bottom at z:" << print_z << ", quality_factor:" << quality_factor << ", resulting layer height:" << height;
#endif  /* ADAPTIVE_LAYER_HEIGHT_DEBUG */
	return height; 
}

// Returns the distance to the next horizontal facet in Z-dir 
// to consider horizontal object features in slice thickness
float SlicingAdaptive::horizontal_facet_distance(float z)
{
	for (size_t i = 0; i < m_faces.size(); ++ i) {
        std::pair<float, float> zspan = m_faces[i].z_span;
        // facet's minimum is higher than max forward distance -> end loop
		if (zspan.first > z + m_slicing_params.max_layer_height)
			break;
		// min_z == max_z -> horizontal facet
		if (zspan.first > z && zspan.first == zspan.second)
			return zspan.first - z;
	}
	
	// objects maximum?
	return (z + (float)m_slicing_params.max_layer_height > (float)m_slicing_params.object_print_z_height()) ? 
		std::max((float)m_slicing_params.object_print_z_height() - z, 0.f) : (float)m_slicing_params.max_layer_height;
}

}; // namespace Slic3r
