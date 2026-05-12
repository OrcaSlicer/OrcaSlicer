#ifndef IGL_ISOLINES_MAP_H
#define IGL_ISOLINES_MAP_H
#include "igl_inline.h"
#include <Eigen/Core>

namespace igl
{
  /// Inject a given colormap with evenly spaced isolines.
  ///
  /// @param[in] CM  #CM by 3 list of colors
  /// @param[in] ico_color  1 by 3 isoline color
  /// @param[in] interval_thickness  number of times to repeat intervals (original colors)
  /// @param[in] iso_thickness  number of times to repeat isoline color (in between
  ///     intervals)
  /// @param[out] ICM  #CM*interval_thickness + (#CM-1)*iso_thickness by 3 list of outputs
  ///     colors
  template <
    typename DerivedCM,
    typename Derivediso_color,
    typename DerivedICM >
  IGL_INLINE void isolines_map(
    const Eigen::MatrixBase<DerivedCM> & CM, 
    const Eigen::MatrixBase<Derivediso_color> & iso_color,
    const int interval_thickness,
    const int iso_thickness,
    Eigen::PlainObjectBase<DerivedICM> & ICM);
  /// \overload
  template <
    typename DerivedCM,
    typename DerivedICM>
  IGL_INLINE void isolines_map(
    const Eigen::MatrixBase<DerivedCM> & CM, 
    Eigen::PlainObjectBase<DerivedICM> & ICM);
}

#ifndef IGL_STATIC_LIBRARY
#  include "isolines_map.cpp"
#endif

#endif
