// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "remove_duplicates.h"
#include <vector>

//template <typename T, typename S>
//IGL_INLINE void igl::remove_duplicates(
//                                 const Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &V,
//                                 const Eigen::Matrix<S, Eigen::Dynamic, Eigen::Dynamic> &F,
//                                 Eigen::Matrix<T, Eigen::Dynamic, Eigen::Dynamic> &NV,
//                                 Eigen::Matrix<S, Eigen::Dynamic, Eigen::Dynamic> &NF,
//                                 Eigen::Matrix<S, Eigen::Dynamic, 1> &I,
//                                 const double epsilon)
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::remove_duplicates(
  const Eigen::PlainObjectBase<DerivedV> &V,
  const Eigen::PlainObjectBase<DerivedF> &F,
  Eigen::PlainObjectBase<DerivedV> &NV,
  Eigen::PlainObjectBase<DerivedF> &NF,
  Eigen::Matrix<typename DerivedF::Scalar, Eigen::Dynamic, 1> &I,
  const double epsilon)
{
  using namespace std;
  //// build collapse map
  int n = V.rows();
  
  I = Eigen::Matrix<typename DerivedF::Scalar, Eigen::Dynamic, 1>(n);
  I[0] = 0;
  
  bool *VISITED = new bool[n];
  for (int i =0; i <n; ++i)
    VISITED[i] = false;
  
  NV.resize(n,V.cols());
  int count = 0;
  Eigen::VectorXd d(n);
  for (int i =0; i <n; ++i)
  {
    if(!VISITED[i])
    {
      NV.row(count) = V.row(i);
      I[i] = count;
      VISITED[i] = true;
      for (int j = i+1; j <n; ++j)
      {
        if((V.row(j) - V.row(i)).norm() < epsilon)
        {
          VISITED[j] = true;
          I[j] = count;
        }
      }
      count ++;
    }
  }
  
  NV.conservativeResize  (  count , Eigen::NoChange );

  count = 0;
  std::vector<typename DerivedF::Scalar> face;
  NF.resizeLike(F);
  for (int i =0; i <F.rows(); ++i)
  {
    face.clear();
    for (int j = 0; j< F.cols(); ++j)
      if(std::find(face.begin(), face.end(), I[F(i,j)]) == face.end())
         face.push_back(I[F(i,j)]);
    if (face.size() == size_t(F.cols()))
    {
      for (unsigned j = 0; j< F.cols(); ++j)
        NF(count,j) = face[j];
      count ++;
    }
  }
  NF.conservativeResize  (  count , Eigen::NoChange );
  
  delete [] VISITED;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::remove_duplicates<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::Matrix<Eigen::Matrix<int, -1, -1, 0, -1, -1>::Scalar, -1, 1, 0, -1, 1>&, double);
template void igl::remove_duplicates<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&, Eigen::Matrix<Eigen::Matrix<int, -1, 3, 1, -1, 3>::Scalar, -1, 1, 0, -1, 1>&, double);
#endif
