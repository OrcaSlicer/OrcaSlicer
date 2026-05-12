#ifndef IGL_COLLAPSE_EDGE_WOULD_CREATE_INTERSECTIONS_H
#define IGL_COLLAPSE_EDGE_WOULD_CREATE_INTERSECTIONS_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Determine if collapse the edge `e` would create new intersections.
  ///
  /// @param[in] e  index into E of edge to try to collapse. E(e,:) = [s d] or [d s] so
  ///     that s<d, then d is collapsed to s.
  /// @param[in] p  dim list of vertex position where to place merged vertex
  /// [mesh inputs]
  /// @param[in,out] V  #V by dim list of vertex positions, lesser index of E(e,:) will be set
  ///     to midpoint of edge.
  /// @param[in,out] F  #F by 3 list of face indices into V.
  /// @param[in,out] E  #E by 2 list of edge indices into V.
  /// @param[in,out] EMAP #F*3 list of indices into E, mapping each directed edge to unique
  ///     unique edge in E
  /// @param[in,out] EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  ///     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  ///     e=(j->i)
  /// @param[in,out] EI  #E by 2 list of edge flap corners (see above).
  /// [mesh inputs]
  /// @param[in] tree AABB tree whose leaves correspond to the current
  ///   (non-null) faces in (V,F)
  ///
  /// \see collapse_edge
  template <typename DerivedV, int DIM> class AABB;

  template <
    typename Derivedp,
    typename DerivedV,
    typename DerivedF,
    typename DerivedE,
    typename DerivedEMAP,
    typename DerivedEF,
    typename DerivedEI>
  IGL_INLINE bool collapse_edge_would_create_intersections(
    const int e,
    const Eigen::MatrixBase<Derivedp> & p,
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const Eigen::MatrixBase<DerivedE> & E,
    const Eigen::MatrixBase<DerivedEMAP> & EMAP,
    const Eigen::MatrixBase<DerivedEF> & EF,
    const Eigen::MatrixBase<DerivedEI> & EI,
    const igl::AABB<DerivedV,3> & tree,
    const int inf_face_id = -1);
}
#ifndef IGL_STATIC_LIBRARY
#  include "collapse_edge_would_create_intersections.cpp"
#endif
#endif
