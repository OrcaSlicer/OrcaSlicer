#ifndef IGL_COPYLEFT_SWEPT_VOLUME_H
#define IGL_COPYLEFT_SWEPT_VOLUME_H
#include "../igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>
namespace igl
{
  namespace copyleft
  {
    // Compute the surface of the swept volume of a solid object with surface
    // (V,F) mesh under going rigid motion.
    // 
    // Inputs:
    //   V  #V by 3 list of mesh positions in reference pose
    //   F  #F by 3 list of mesh indices into V
    //   transform  function handle so that transform(t) returns the rigid
    //     transformation at time t∈[0,1]
    //   steps  number of time steps: steps=3 --> t∈{0,0.5,1}
    //   grid_res  number of grid cells on the longest side containing the
    //     motion (isolevel+1 cells will also be added on each side as padding)
    //   isolevel  distance level to be contoured as swept volume
    // Outputs:
    //   SV  #SV by 3 list of mesh positions of the swept surface
    //   SF  #SF by 3 list of mesh faces into SV
    IGL_INLINE void swept_volume(
      const Eigen::MatrixXd & V,
      const Eigen::MatrixXi & F,
      const std::function<Eigen::Affine3d(const double t)> & transform,
      const size_t steps,
      const size_t grid_res,
      const size_t isolevel,
      Eigen::MatrixXd & SV,
      Eigen::MatrixXi & SF);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "swept_volume.cpp"
#endif

#endif
