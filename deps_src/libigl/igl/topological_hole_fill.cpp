// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Zhongshi Jiang <jiangzs@nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "topological_hole_fill.h"
  template <
  typename DerivedF,
  typename VectorIndex,
  typename DerivedF_filled>
IGL_INLINE void igl::topological_hole_fill(
  const Eigen::MatrixBase<DerivedF> & F,
  const std::vector<VectorIndex> & holes,
  Eigen::PlainObjectBase<DerivedF_filled> &F_filled)
{
  int n_filled_faces = 0;
  int num_holes = holes.size();
  int real_F_num = F.rows();
  const int V_rows = F.maxCoeff()+1;

  for (int i = 0; i < num_holes; i++)
    n_filled_faces += holes[i].size();
  F_filled.resize(n_filled_faces + real_F_num, 3);
  F_filled.topRows(real_F_num) = F;

  int new_vert_id = V_rows;
  int new_face_id = real_F_num;

  for (int i = 0; i < num_holes; i++, new_vert_id++)
  {
    int it = 0;
    int back = holes[i].size() - 1;
    F_filled.row(new_face_id++) << holes[i][it], holes[i][back], new_vert_id;
    while (it != back)
    {
      F_filled.row(new_face_id++)
          << holes[i][(it + 1)],
          holes[i][(it)], new_vert_id;
      it++;
    }
  }
  assert(new_face_id == F_filled.rows());
  assert(new_vert_id == V_rows + num_holes);

}

#ifdef IGL_STATIC_LIBRARY
template void igl::topological_hole_fill<Eigen::Matrix<int, -1, -1, 0, -1, -1>, std::vector<int, std::allocator<int> > >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
