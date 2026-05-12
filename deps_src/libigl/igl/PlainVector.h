// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2024 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_PLAINVECTOR_H
#define IGL_PLAINVECTOR_H
#include <Eigen/Core>
#include "PlainMatrix.h"

namespace igl
{
  // PlainVectorHelper to determine correct matrix type based on Derived and Size
  template <typename Derived, int Size, int Options>
    struct PlainVectorHelper {
      // Conditional Type: Column vector if is_column_vector is true, otherwise row vector
      using Type = Eigen::Matrix<
        typename Derived::Scalar,
        (Derived::ColsAtCompileTime == 1 && Derived::RowsAtCompileTime != 1) ? Size : 1,
        (Derived::ColsAtCompileTime == 1 && Derived::RowsAtCompileTime != 1) ? 1 : Size,
        Options>;
    };

  /// \see PlainMatrix
  template <
    typename Derived, 
    int Size = (Derived::ColsAtCompileTime == 1 && Derived::RowsAtCompileTime != 1) ? Derived::RowsAtCompileTime : Derived::ColsAtCompileTime,
    int Options = get_options<Derived>::value>
  using PlainVector = typename PlainVectorHelper<Derived, Size, Options>::Type;

}
#endif

