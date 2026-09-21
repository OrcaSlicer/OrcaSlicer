#ifndef slic3r_FillSpiralInset_hpp_
#define slic3r_FillSpiralInset_hpp_

#include "FillBase.hpp"

namespace Slic3r {

class FillSpiralInset : public Fill
{
public:
	~FillSpiralInset() override = default;
	bool is_self_crossing() override { return false; }

protected:
	Fill* clone() const override { return new FillSpiralInset(*this); };
	void _fill_surface_single(
		const FillParams              &params,
		unsigned int                   thickness_layers,
		const std::pair<float, Point> &direction,
		ExPolygon                      expolygon,
		Polylines                     &polylines_out) override;

	// Orca: solid surfaces are filled with Arachne's variable width walls, which widen to take up
	// whatever the fixed width loops above would have left over as gaps.
	void _fill_surface_single(
		const FillParams              &params,
		unsigned int                   thickness_layers,
		const std::pair<float, Point> &direction,
		ExPolygon                      expolygon,
		ThickPolylines                &thick_polylines_out) override;

	bool no_sort() const override { return true; }
};

} // namespace Slic3r

#endif // slic3r_FillSpiralInset_hpp_
