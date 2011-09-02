// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "faces_first.h"

#include <vector>
#include <Eigen/Dense>

template <typename MatV, typename MatF, typename VecI>
IGL_INLINE void igl::faces_first(
  const MatV & V, 
  const MatF & F, 
  MatV & RV, 
  MatF & RF, 
  VecI & IM)
{
  assert(&V != &RV);
  assert(&F != &RF);
  using namespace std;
  using namespace Eigen;
  vector<bool> in_face(V.rows());
  for(int i = 0; i<F.rows(); i++)
  {
    for(int j = 0; j<F.cols(); j++)
    {
      in_face[F(i,j)] = true;
    }
  }
  // count number of vertices not in faces
  int num_in_F = 0;
  for(int i = 0;i<V.rows();i++)
  {
    num_in_F += (in_face[i]?1:0);
  }
  // list of unique vertices that occur in F
  VectorXi U(num_in_F);
  // list of unique vertices that do not occur in F
  VectorXi NU(V.rows()-num_in_F);
  int Ui = 0;
  int NUi = 0;
  // loop over vertices
  for(int i = 0;i<V.rows();i++)
  {
    if(in_face[i])
    {
      U(Ui) = i;
      Ui++;
    }else
    {
      NU(NUi) = i;
      NUi++;
    }
  }
  IM.resize(V.rows());
  // reindex vertices that occur in faces to be first
  for(int i = 0;i<U.size();i++)
  {
    IM(U(i)) = i;
  }
  // reindex vertices that do not occur in faces to come after those that do
  for(int i = 0;i<NU.size();i++)
  {
    IM(NU(i)) = i+U.size();
  }
  RF.resizeLike(F);
  // Reindex faces
  for(int i = 0; i<F.rows(); i++)
  {
    for(int j = 0; j<F.cols(); j++)
    {
      RF(i,j) = IM(F(i,j));
    }
  }
  RV.resizeLike(V);
  // Reorder vertices
  for(int i = 0;i<V.rows();i++)
  {
    RV.row(IM(i)) = V.row(i);
  }
}

template <typename MatV, typename MatF, typename VecI>
IGL_INLINE void igl::faces_first(
  MatV & V, 
  MatF & F, 
  VecI & IM)
{
  MatV RV;
  // Copying F may not be needed, seems RF = F is safe (whereas RV = V is not)
  MatF RF;
  igl::faces_first(V,F,RV,RF,IM);
  V = RV;
  F = RF;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::faces_first<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::Matrix<double, -1, -1, 0, -1, -1>&, Eigen::Matrix<int, -1, -1, 0, -1, -1>&, Eigen::Matrix<int, -1, 1, 0, -1, 1>&);
#endif
