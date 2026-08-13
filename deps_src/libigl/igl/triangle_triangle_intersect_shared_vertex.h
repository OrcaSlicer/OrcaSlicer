#ifndef IGL_TRIANGLE_TRIANGLE_INTERSECT_SHARED_VERTEX_H
#define IGL_TRIANGLE_TRIANGLE_INTERSECT_SHARED_VERTEX_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Determine whether two triangles --- which share a vertex F(f,sf) ==
  /// F(g,sg) --- intersect. We consider the `f`th and `g`th triangles in `F`
  /// indexing rows of `V` for 3D positions, but the `c`th corner (opposite the
  /// shared edge) of the `f`th triangle is replaced by `p`. 
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices into rows of V
  /// @param[in] f  index into F of first triangle
  /// @param[in] sf  corner index of shared vertex in first triangle
  /// @param[in] c  index into F of corner opposite shared edge (assumed c≠sf)
  /// @param[in] p  3D position to replace cth corner of first triangle
  /// @param[in] g  index into F of second triangle
  /// @param[in] sg  corner index of shared vertex in second triangle
  /// @param[in] epsilon  tolerance used to determine intersection
  /// @returns  true if triangles intersect
  ///
  /// \see edge_flaps, tri_tri_intersect, triangle_triangle_intersect,
  /// triangle_triangle_intersect_shared_edge
  ///
  /// \pre both faces are assumed to have non-trivial area
  template <
    typename DerivedV,
    typename DerivedF,
    typename Derivedp>
  IGL_INLINE bool triangle_triangle_intersect_shared_vertex(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const int f,
    const int sf,
    const int c,
    const Eigen::MatrixBase<Derivedp> & p,
    const int g,
    const int sg,
    const typename DerivedV::Scalar epsilon);
}

#ifndef IGL_STATIC_LIBRARY
#  include "triangle_triangle_intersect_shared_vertex.cpp"
#endif

#endif
