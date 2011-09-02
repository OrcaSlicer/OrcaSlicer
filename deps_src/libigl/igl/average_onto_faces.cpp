// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "average_onto_faces.h"

template <typename T, typename I>
IGL_INLINE void igl::average_onto_faces(const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &V,
            const Eigen::Matrix<I, Eigen::Dynamic, Eigen::Dynamic> &F,
            const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &S,
            Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &SF)
{
  
  SF = Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic>::Zero(F.rows(),S.cols());

  for (int i = 0; i <F.rows(); ++i)
    for (int j = 0; j<F.cols(); ++j)
      SF.row(i) += S.row(F(i,j));

  SF.array() /= F.cols();
  
};

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
