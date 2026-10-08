#ifndef slic3r_Fill3DHoneycomb_hpp_
#define slic3r_Fill3DHoneycomb_hpp_

#include <map>
#include <utility>

#include "../libslic3r.h"

#include "libslic3r/ExPolygon.hpp"
#include "FillBase.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r { class Point; }

namespace Slic3r {

class Fill3DHoneycomb : public Fill
{
public:
    Fill* clone() const override { return new Fill3DHoneycomb(*this); };
    ~Fill3DHoneycomb() override {}

    // note: updated 3D Honeycomb doesn't need bridge flow because the
    //       pattern is placed on top of previous layers
    bool use_bridge_flow() const override { return false; }
    bool is_self_crossing() override { return false; }

protected:
	void _fill_surface_single(
	    const FillParams                &params, 
	    unsigned int                     thickness_layers,
	    const std::pair<float, Point>   &direction, 
	    ExPolygon                 		 expolygon,
	    Polylines                       &polylines_out) override;
};

} // namespace Slic3r

#endif // slic3r_Fill3DHoneycomb_hpp_
