// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "bfs_orient.h"
#include "orientable_patches.h"
#include "parallel_for.h"
#include <Eigen/Sparse>
#include <queue>

template <typename DerivedF, typename DerivedFF, typename DerivedC>
IGL_INLINE void igl::bfs_orient(
  const Eigen::MatrixBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedFF> & FF,
  Eigen::PlainObjectBase<DerivedC> & C)
{
  using namespace Eigen;
  using namespace std;
  SparseMatrix<typename DerivedF::Scalar> A;
  orientable_patches(F,C,A);

  // number of faces
  const int m = F.rows();
  // number of patches
  const int num_cc = C.maxCoeff()+1;
  VectorXi seen = VectorXi::Zero(m);

  // Edge sets
  const int ES[3][2] = {{1,2},{2,0},{0,1}};

  if(((void*)&FF) != ((void*)&F))
  {
    FF = F;
  }
  // loop over patches
  parallel_for(num_cc,[&](const int c)
  {
    queue<typename DerivedF::Scalar> Q;
    // find first member of patch c
    for(int f = 0;f<FF.rows();f++)
    {
      if(C(f) == c)
      {
        Q.push(f);
        break;
      }
    }
    assert(!Q.empty());
    while(!Q.empty())
    {
      const typename DerivedF::Scalar f = Q.front();
      Q.pop();
      if(seen(f) > 0)
      {
        continue;
      }
      seen(f)++;
      // loop over neighbors of f
      for(typename SparseMatrix<typename DerivedF::Scalar>::InnerIterator it (A,f); it; ++it)
      {
        // might be some lingering zeros, and skip self-adjacency
        if(it.value() != 0 && it.row() != f)
        {
          const int n = it.row();
          assert(n != f);
          // loop over edges of f
          for(int efi = 0;efi<3;efi++)
          {
            // efi'th edge of face f
            Vector2i ef(FF(f,ES[efi][0]),FF(f,ES[efi][1]));
            // loop over edges of n
            for(int eni = 0;eni<3;eni++)
            {
              // eni'th edge of face n
              Vector2i en(FF(n,ES[eni][0]),FF(n,ES[eni][1]));
              // Match (half-edges go same direction)
              if(ef(0) == en(0) && ef(1) == en(1))
              {
                // flip face n
                FF.row(n) = FF.row(n).reverse().eval();
              }
            }
          }
          // add neighbor to queue
          Q.push(n);
        }
      }
    }
  },1000);

  // make sure flip is OK if &FF = &F
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::bfs_orient<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
