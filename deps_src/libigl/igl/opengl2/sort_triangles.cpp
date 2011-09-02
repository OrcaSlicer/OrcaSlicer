// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "sort_triangles.h"
#include "project.h"
#include "../sort_triangles.h"
#include "gl.h"
#include "../sort.h"
#include "../slice.h"
#include "../barycenter.h"
#include <iostream>
template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedFF,
  typename DerivedI>
void igl::opengl2::sort_triangles(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedFF> & FF,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  using namespace Eigen;
  using namespace std;
  // Put model, projection, and viewport matrices into double arrays
  Matrix4d MV;
  Matrix4d P;
  glGetDoublev(GL_MODELVIEW_MATRIX,  MV.data());
  glGetDoublev(GL_PROJECTION_MATRIX, P.data());
  if(V.cols() == 3)
  {
    Matrix<typename DerivedV::Scalar, DerivedV::RowsAtCompileTime,4> hV;
    hV.resize(V.rows(),4);
    hV.block(0,0,V.rows(),V.cols()) = V;
    hV.col(3).setConstant(1);
    return igl::sort_triangles(hV,F,MV,P,FF,I);
  }else
  {
    return igl::sort_triangles(V,F,MV,P,FF,I);
  }
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedFF,
  typename DerivedI>
void igl::opengl2::sort_triangles_slow(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedFF> & FF,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  using namespace Eigen;
  using namespace std;
  // Barycenter, centroid
  Eigen::Matrix<typename DerivedV::Scalar, DerivedF::RowsAtCompileTime,1> D,sD;
  Eigen::Matrix<typename DerivedV::Scalar, DerivedF::RowsAtCompileTime,3> BC;
  D.resize(F.rows(),3);
  barycenter(V,F,BC);
  for(int f = 0;f<F.rows();f++)
  {
    Eigen::Matrix<typename DerivedV::Scalar, 3,1> bc,pbc;
    bc = BC.row(f);
    project(bc,pbc);
    D(f) = pbc(2);
  }
  sort(D,1,false,sD,I);
  slice(F,I,1,FF);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::opengl2::sort_triangles<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::opengl2::sort_triangles_slow<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
