#ifndef IGL_COPYLEFT_CGAL_IS_SELF_INTERSECTING_H
#define IGL_COPYLEFT_CGAL_IS_SELF_INTERSECTING_H

#include "../../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Determine if a mesh has _any_ self-intersections. Skips any
      /// `(IGL_COLLAPSE_EDGE_NULL,IGL_COLLAPSE_EDGE_NULL,IGL_COLLAPSE_EDGE_NULL)`
      /// faces and returns true if any faces have zero area.
      /// 
      /// @param[in] V  #V by 3 list of vertex positions
      /// @param[in] F  #F by 3 list of triangle indices into V
      /// @return true if any faces intersect
      ///
      /// \see remesh_self_intersections, SelfIntersectMesh
      template <
        typename DerivedV,
        typename DerivedF>
      bool is_self_intersecting(
        const Eigen::MatrixBase<DerivedV> & V,
        const Eigen::MatrixBase<DerivedF> & F);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "is_self_intersecting.cpp"
#endif

#endif
