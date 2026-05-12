#ifndef IGL_OFFSET_SURFACE_H
#define IGL_OFFSET_SURFACE_H
#include "igl_inline.h"
#include "signed_distance.h"
#include <Eigen/Core>

namespace igl
{
  /// Compute a triangulated offset surface using matching cubes on a grid of
  /// signed distance values from the input triangle mesh.
  ///
  /// @param[in] V  #V by 3 list of mesh vertex positions
  /// @param[in] F  #F by 3 list of mesh triangle indices into V
  /// @param[in] isolevel  iso level to extract (signed distance: negative inside)
  /// @param[in] s  number of grid cells along longest side (controls resolution)
  /// @param[in] signed_distance_type  type of signing to use (see
  ///     ../signed_distance.h)
  /// @param[out] SV  #SV by 3 list of output surface mesh vertex positions
  /// @param[out] SF  #SF by 3 list of output mesh triangle indices into SV
  /// @param[out] GV  #GV=side(0)*side(1)*side(2) by 3 list of grid cell centers
  /// @param[out] side  list of number of grid cells in x, y, and z directions
  /// @param[out] S  #GV by 3 list of signed distance values _near_ `isolevel` ("far"
  ///     from `isolevel` these values are incorrect)
  ///
  template <
    typename DerivedV,
    typename DerivedF,
    typename isolevelType,
    typename DerivedSV,
    typename DerivedSF,
    typename DerivedGV,
    typename Derivedside,
    typename DerivedS>
  void offset_surface(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const isolevelType isolevel,
    const typename Derivedside::Scalar s,
    const SignedDistanceType & signed_distance_type,
    Eigen::PlainObjectBase<DerivedSV> & SV,
    Eigen::PlainObjectBase<DerivedSF> & SF,
    Eigen::PlainObjectBase<DerivedGV> & GV,
    Eigen::PlainObjectBase<Derivedside> & side,
    Eigen::PlainObjectBase<DerivedS> & S);
  
}
#ifndef IGL_STATIC_LIBRARY
#  include "offset_surface.cpp"
#endif 
#endif 
