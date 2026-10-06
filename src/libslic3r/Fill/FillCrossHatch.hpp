#ifndef slic3r_FillCrossHatch_hpp_
#define slic3r_FillCrossHatch_hpp_

#include <map>
#include <utility>


#include "libslic3r/ExPolygon.hpp"
#include "FillBase.hpp"
#include "libslic3r/Polyline.hpp"

namespace Slic3r { class Point; }

namespace Slic3r {

class FillCrossHatch : public Fill
{
public:
    Fill *clone() const override { return new FillCrossHatch(*this); };
    ~FillCrossHatch() override {}
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

#endif // slic3r_FillCrossHatch_hpp_
