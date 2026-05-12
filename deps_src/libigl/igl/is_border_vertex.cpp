// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "is_border_vertex.h"
#include <vector>

#include "triangle_triangle_adjacency.h"

template <typename DerivedF>
IGL_INLINE std::vector<bool> igl::is_border_vertex(
  const Eigen::MatrixBase<DerivedF> &F)
{
  assert(F.cols() == 3 && "Only triangle meshes are supported");
  Eigen::Matrix<typename DerivedF::Scalar, Eigen::Dynamic, Eigen::Dynamic> FF;
  igl::triangle_triangle_adjacency(F,FF);
  std::vector<bool> ret(F.maxCoeff()+1);
  for(unsigned i=0; i<ret.size();++i)
    ret[i] = false;

  for(unsigned i=0; i<F.rows();++i)
    for(unsigned j=0;j<F.cols();++j)
      if(FF(i,j) == -1)
      {
        ret[F(i,j)]       = true;
        ret[F(i,(j+1)%F.cols())] = true;
      }
  return ret;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template std::vector<bool, std::allocator<bool> > igl::is_border_vertex<Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
template std::vector<bool, std::allocator<bool> > igl::is_border_vertex<Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&);
#endif
