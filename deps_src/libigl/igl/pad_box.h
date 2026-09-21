#ifndef IGL_PAD_BOX_H
#define IGL_PAD_BOX_H

#include <Eigen/Geometry>
#include "igl_inline.h"

namespace igl
{
  /// Pads a box by a given amount
  ///
  /// @param[in] pad: amount to pad each side of the box
  /// @param[in,out] box  box to be padded.
  template <typename Scalar, int DIM>
  IGL_INLINE void pad_box(
    const Scalar pad,
    Eigen::AlignedBox<Scalar,DIM> & box);
}

#ifndef IGL_STATIC_LIBRARY
#  include "pad_box.cpp"
#endif

#endif
