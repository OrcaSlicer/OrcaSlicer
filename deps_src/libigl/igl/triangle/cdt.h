#ifndef IGL_TRIANGLE_CDT_H
#define IGL_TRIANGLE_CDT_H

#include "../igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  namespace triangle
  {
    /// Construct the constrained delaunay triangulation of the convex hull
    /// of a given set of points and segments in 2D. This differs from a direct
    /// call to triangulate because it will preprocess the input to remove
    /// duplicates and return an adjusted segment list on the output.
    /// 
    /// @param[in] V  #V by 2 list of texture mesh vertices
    /// @param[in] E  #E by 2 list of constraint edge indices into V
    /// @param[in] flags  string of triangle flags should contain "-c" unless the
    ///     some subset of segments are known to enclose all other
    ///     points/segments.
    /// @param[out] WV  #WV by 2 list of background mesh vertices 
    /// @param[out] WF  #WF by 2 list of background mesh triangle indices into WV
    /// @param[out] WE  #WE by 2 list of constraint edge indices into WV (might be smaller
    ///     than E because degenerate constraints have been removed)
    /// @param[out] J  #V list of indices into WF/WE for each vertex in V
    ///
    template <
      typename DerivedV, 
      typename DerivedE,
      typename DerivedWV,
      typename DerivedWF,
      typename DerivedWE,
      typename DerivedJ>
    IGL_INLINE void cdt(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedE> & E,
      const std::string & flags,
      Eigen::PlainObjectBase<DerivedWV> & WV,
      Eigen::PlainObjectBase<DerivedWF> & WF,
      Eigen::PlainObjectBase<DerivedWE> & WE,
      Eigen::PlainObjectBase<DerivedJ> & J);
  }
}

#ifndef IGL_STATIC_LIBRARY
#include "cdt.cpp"
#endif
#endif
