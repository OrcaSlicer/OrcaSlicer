#ifndef IGL_RAMER_DOUGLAS_PEUCKER_H
#define IGL_RAMER_DOUGLAS_PEUCKER_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  // Ramer-Douglas-Peucker piecewise-linear curve simplification.
  //
  // Inputs:
  //   P  #P by dim ordered list of vertices along the curve
  //   tol  tolerance (maximal euclidean distance allowed between the new line
  //     and a vertex)
  // Outputs:
  //   S  #S by dim ordered list of points along the curve
  //   J  #S list of indices into P so that S = P(J,:)
  template <typename DerivedP, typename DerivedS, typename DerivedJ>
  IGL_INLINE void ramer_douglas_peucker(
    const Eigen::MatrixBase<DerivedP> & P,
    const typename DerivedP::Scalar tol,
    Eigen::PlainObjectBase<DerivedS> & S,
    Eigen::PlainObjectBase<DerivedJ> & J);
  // Run (Ramer-)Duglass-Peucker curve simplification but keep track of where
  // every point on the original curve maps to on the simplified curve.
  //
  // Inputs:
  //   P  #P by dim list of points, (use P([1:end 1],:) for loops)
  //   tol  DP tolerance
  // Outputs:
  //   S  #S by dim list of points along simplified curve
  //   J  #S indices into P of simplified points
  //   Q  #P by dim list of points mapping along simplified curve
  //
  template <
    typename DerivedP, 
    typename DerivedS, 
    typename DerivedJ,
    typename DerivedQ>
  IGL_INLINE void ramer_douglas_peucker(
    const Eigen::MatrixBase<DerivedP> & P,
    const typename DerivedP::Scalar tol,
    Eigen::PlainObjectBase<DerivedS> & S,
    Eigen::PlainObjectBase<DerivedJ> & J,
    Eigen::PlainObjectBase<DerivedQ> & Q);

}
#ifndef IGL_STATIC_LIBRARY
#  include "ramer_douglas_peucker.cpp"
#endif
#endif
