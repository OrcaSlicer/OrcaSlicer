#ifndef IGL_BOX_SURFACE_AREA_H
#define IGL_BOX_SURFACE_AREA_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <Eigen/Geometry>

namespace igl
{
  /// Compute the surface area of a box given its min and max corners
  /// 
  /// @param[in] min_corner  #d vector of min corner position
  /// @param[in] max_corner  #d vector of max corner position
  /// @return            surface area of box
  template <typename DerivedCorner>
  IGL_INLINE typename DerivedCorner::Scalar box_surface_area(
    const Eigen::MatrixBase<DerivedCorner> & min_corner,
    const Eigen::MatrixBase<DerivedCorner> & max_corner);
  /// \overload
  /// @param[in] box  axis-aligned bounding box
  template <typename Scalar, int AmbientDim>
  IGL_INLINE Scalar box_surface_area(
    const Eigen::AlignedBox<Scalar,AmbientDim> & box);
}

#ifndef IGL_STATIC_LIBRARY
#  include "box_surface_area.cpp"
#endif

#endif

