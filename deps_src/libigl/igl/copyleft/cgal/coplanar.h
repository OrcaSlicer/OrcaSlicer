#ifndef IGL_COPYLEFT_CGAL_COPLANAR_H
#define IGL_COPYLEFT_CGAL_COPLANAR_H
#include "../../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Test whether all points are on same plane.
      ///
      /// @param[in] V  #V by 3 list of 3D vertex positions
      /// @return true if all points lie on the same plane
      template <typename DerivedV>
      IGL_INLINE bool coplanar(
        const Eigen::MatrixBase<DerivedV> & V);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "coplanar.cpp"
#endif

#endif
