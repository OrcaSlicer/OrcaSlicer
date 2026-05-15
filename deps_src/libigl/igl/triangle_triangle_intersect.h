#ifndef IGL_TRIANGLE_TRIANGLE_INTERSECT_H
#define IGL_TRIANGLE_TRIANGLE_INTERSECT_H

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Determine whether two triangles intersect. We consider the `f`th and `g`th
  /// triangles in `F` indexing rows of `V` for 3D positions, but the `c`th corner
  /// of the `f`th triangle is replaced by `p`. In matlab, this would be
  ///
  /// ```matlab
  /// Tf = V(F(f,:),:);
  /// Tf(c,:) = p;
  /// ```
  ///
  /// and 
  ///
  /// ```matlab
  /// Tg = V(F(g,:),:);
  /// ```
  ///
  /// Triangles can share an edge, but only if it's the one opposite the replaced
  /// corner. 
  ///
  /// @param[in] V  #V by 3 list of vertex positions
  /// @param[in] F  #F by 3 list of triangle indices into rows of V
  /// @param[in] E #E by 2 list of unique undirected edge indices into rows of V
  /// @param[in] EMAP  #F*3 list of indices into F, mapping each directed edge to
  ///   unique edge in {1,...,E}
  /// @param[in] EF  #E by 2 list of edge indices into F
  /// @param[in] f  index into F of first triangle
  /// @param[in] c  index into F of corner of first triangle to replace with `p`
  /// @param[in] p  3D position to replace corner of first triangle
  /// @param[in] g  index into F of second triangle
  /// @returns  true if triangles intersect
  ///
  /// \note This very specialized function should be complemented with the more
  /// general functions in igl::tri_tri_intersect (which should be renamed and
  /// split into appropriate files). There's a reasonably amount of shared code
  /// with igl::copyleft::cgal::remesh_self_intersections too.
  ///
  /// \see edge_flaps, tri_tri_intersect,
  /// predicates::triangle_triangle_intersect
  template <
    typename DerivedV,
    typename DerivedF,
    typename DerivedE,
    typename DerivedEMAP,
    typename DerivedEF,
    typename Derivedp>
  IGL_INLINE bool triangle_triangle_intersect(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedE> & E,
    const Eigen::MatrixBase<DerivedEMAP> & EMAP,
    const Eigen::MatrixBase<DerivedEF> & EF,
    const int f,
    const int c,
    const Eigen::MatrixBase<Derivedp> & p,
    const int g);
  /// Assuming that we've computed a list of non-coplanar intersecting triangle
  /// pairs between (V1,F1) and (V2,F2), compute the 
  ///
  /// @param[in] V1 #V1 by 3 list of vertex positions
  /// @param[in] F1 #F1 by 3 list of triangle indices into rows of V1
  /// @param[in] V2 #V2 by 3 list of vertex positions
  /// @param[in] F2 #F2 by 3 list of triangle indices into rows of V2
  /// @param[in] IF #IF by 2 list of intersecting triangle pairs so that IF(i,:)
  ///   [f1 f2] means that the f1th triangle in F1 intersects the f2th triangle in
  ///   F2 and that they're non-coplanar
  /// @param[out] EV #EV by 3 list of vertex positions of intersection segments
  /// @param[out] EE #EE by 2 list of edge indices into EV
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedIF,
    typename DerivedEV,
    typename DerivedEE>
  IGL_INLINE void triangle_triangle_intersect(
      const Eigen::MatrixBase<DerivedV> & V1,
      const Eigen::MatrixBase<DerivedF> & F1,
      const Eigen::MatrixBase<DerivedV> & V2,
      const Eigen::MatrixBase<DerivedF> & F2,
      const Eigen::MatrixBase<DerivedIF> & IF,
      Eigen::PlainObjectBase<DerivedEV> & EV,
      Eigen::PlainObjectBase<DerivedEE> & EE);
  /// \overload
  /// \brief Wrapper for triangle_triangle_intersect with self-intersections
  template <
    typename DerivedV, 
    typename DerivedF, 
    typename DerivedIF,
    typename DerivedEV,
    typename DerivedEE>
  IGL_INLINE void triangle_triangle_intersect(
      const Eigen::MatrixBase<DerivedV> & V,
      const Eigen::MatrixBase<DerivedF> & F,
      const Eigen::MatrixBase<DerivedIF> & IF,
      Eigen::PlainObjectBase<DerivedEV> & EV,
      Eigen::PlainObjectBase<DerivedEE> & EE);
}

#ifndef IGL_STATIC_LIBRARY
#  include "triangle_triangle_intersect.cpp"
#endif

#endif
