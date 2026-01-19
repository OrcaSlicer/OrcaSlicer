#ifndef slic3r_ExtrusionSimulator_hpp_
#define slic3r_ExtrusionSimulator_hpp_

#include "libslic3r.h"
#include "ExtrusionEntity.hpp"
#include "BoundingBox.hpp"

namespace Slic3r {

enum ExtrusionSimulationType
{
    ExtrusionSimulationSimple,
    ExtrusionSimulationDontSpread,
    ExtrisopmSimulationSpreadNotOverfilled,
    ExtrusionSimulationSpreadFull,
    ExtrusionSimulationSpreadExcess
};

// An opaque class, to keep the boost stuff away from the header.
class ExtrusionSimulatorImpl;

class ExtrusionSimulator
{
public:
    ExtrusionSimulator();
    ~ExtrusionSimulator();

    // Size of the image, that will be returned by image_ptr().
    // The image may be bigger than the viewport as many graphics drivers 
    // expect the size of a texture to be rounded to a power of two.
    void  		set_image_size(const Point &image_size);
    // Which part of the image shall be rendered to?
    void  		set_viewport(const BoundingBox &viewport);
    // Shift and scale of the rendered extrusion paths into the viewport.
    void		set_bounding_box(const BoundingBox &bbox);

    // Reset the extrusion accumulator to zero for all buckets.
    void		reset_accumulator();
    // Paint a thick path into an extrusion buffer.
    // A simple implementation is provided now, splatting a rectangular extrusion for each linear segment.
    // In the future, spreading and suqashing of a material will be simulated.
    void		extrude_to_accumulator(const ExtrusionPath &path, const Point &shift, ExtrusionSimulationType simulationType);
    // Evaluate the content of the accumulator and paint it into the viewport.
    // After this call the image_ptr() call will return a valid image.
    void		evaluate_accumulator(ExtrusionSimulationType simulationType);
    // An RGBA image of image_size, to be loaded into a GPU texture.
    const void* image_ptr() const;

private:
	Point					    	image_size;
	BoundingBox				    	viewport;
	BoundingBox 					bbox;

	ExtrusionSimulatorImpl		   *pimpl;
};

}

#endif /* slic3r_ExtrusionSimulator_hpp_ */
