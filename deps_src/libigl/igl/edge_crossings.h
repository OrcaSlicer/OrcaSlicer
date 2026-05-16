#ifndef IGL_EDGE_CROSSINGS_H
#define IGL_EDGE_CROSSINGS_H

#include "igl_inline.h"
#include <Eigen/Core>
#include <unordered_map>

namespace igl
{
  /// Compute the each point that a scalar field crosses a specified value along
  /// an edge of a mesh.
  ///
  /// @param[in] uE  #E by 2 list of edge indices
  /// @param[in] S  #V by 1 list of scalar field values
  /// @param[in] val  value to check for crossings
  /// @param[out] uE2I #T map from edge index to index in T
  /// @param[out] T  #T by 1 list of parametric coordinates of crossings
  ///
  /// \see isolines, isolines_intrinsic
  template <
    typename DeriveduE,
    typename DerivedS,
    typename DerivedT>
  void edge_crossings(
    const Eigen::MatrixBase<DeriveduE> & uE,
    const Eigen::MatrixBase<DerivedS> & S,
    const typename DerivedS::Scalar val,
    std::unordered_map<int,int> & uE2I,
    Eigen::PlainObjectBase<DerivedT> & T);
}

#ifndef IGL_STATIC_LIBRARY
#  include "edge_crossings.cpp"
#endif

#endif
