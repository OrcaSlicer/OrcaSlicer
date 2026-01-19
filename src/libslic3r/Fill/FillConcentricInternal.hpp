#ifndef slic3r_FillConcentricInternal_hpp_
#define slic3r_FillConcentricInternal_hpp_

#include "FillBase.hpp"

namespace Slic3r {

class FillConcentricInternal : public Fill
{
public:
    ~FillConcentricInternal() override = default;
    void fill_surface_extrusion(const Surface *surface, const FillParams &params, ExtrusionEntitiesPtr &out) override;
    bool is_self_crossing() override { return false; }

protected:
    Fill* clone() const override { return new FillConcentricInternal(*this); };
    bool no_sort() const override { return true; }

    friend class Layer;
};

} // namespace Slic3r

#endif // slic3r_FillConcentricInternal_hpp_
