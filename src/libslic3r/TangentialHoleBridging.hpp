#ifndef slic3r_TangentialHoleBridging_hpp_
#define slic3r_TangentialHoleBridging_hpp_

namespace Slic3r {

class PrintObject;
class LayerRegion;

class TangentialHoleBridging {
public:
    // ORCA: Main entry point to apply the tangential sacrificial bridging
    // Generates crossed struts for counterbore holes.
    static void apply(PrintObject* print_object);
    

};

} // namespace Slic3r

#endif // slic3r_TangentialHoleBridging_hpp_
