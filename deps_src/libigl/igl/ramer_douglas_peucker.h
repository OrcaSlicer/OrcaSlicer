#ifndef IGL_RAMER_DOUGLAS_PEUCKER_H
#define IGL_RAMER_DOUGLAS_PEUCKER_H
#include "igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  /// Ramer-Douglas-Peucker piecewise-linear curve simplification.
  ///
  /// @param[in] P  #P by dim ordered list of vertices along the curve
  /// @param[in] tol  tolerance (maximal euclidean distance allowed between the new line
  ///     and a vertex)
  /// @param[out] S  #S by dim ordered list of points along the curve
  /// @param[out] J  #S list of indices into P so that S = P(J,:)
  template <typename DerivedP, typename DerivedS, typename DerivedJ>
  IGL_INLINE void ramer_douglas_peucker(
    const Eigen::MatrixBase<DerivedP> & P,
    const typename DerivedP::Scalar tol,
    Eigen::PlainObjectBase<DerivedS> & S,
    Eigen::PlainObjectBase<DerivedJ> & J);
  /// \overload
  /// \brief Run Ramer-Douglas-Peucker curve simplification but keep track of
  /// where every point on the original curve maps to on the simplified curve.
  ///
  /// @param[out] Q  #P by dim list of points mapping along simplified curve
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
