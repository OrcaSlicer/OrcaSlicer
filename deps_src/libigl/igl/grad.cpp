// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "grad.h"
#include <Eigen/Geometry>
#include <vector>

#include "PI.h"
#include "per_face_normals.h"
#include "volume.h"
#include "doublearea.h"

template <typename DerivedV, typename DerivedF>
IGL_INLINE void grad_tet(const Eigen::PlainObjectBase<DerivedV>&V,
                     const Eigen::PlainObjectBase<DerivedF>&T,
                            Eigen::SparseMatrix<typename DerivedV::Scalar> &G,
                            bool uniform) {
  using namespace Eigen;
  assert(T.cols() == 4);
  const int n = V.rows(); int m = T.rows();

  /*
      F = [ ...
      T(:,1) T(:,2) T(:,3); ...
      T(:,1) T(:,3) T(:,4); ...
      T(:,1) T(:,4) T(:,2); ...
      T(:,2) T(:,4) T(:,3)]; */
  MatrixXi F(4*m,3);
  for (int i = 0; i < m; i++) {
    F.row(0*m + i) << T(i,0), T(i,1), T(i,2);
    F.row(1*m + i) << T(i,0), T(i,2), T(i,3);
    F.row(2*m + i) << T(i,0), T(i,3), T(i,1);
    F.row(3*m + i) << T(i,1), T(i,3), T(i,2);
  }
  // compute volume of each tet
  Eigen::Matrix<typename DerivedV::Scalar, Eigen::Dynamic, 1> vol; 
  igl::volume(V,T,vol);

  Eigen::Matrix<typename DerivedV::Scalar, Eigen::Dynamic, 1> A(F.rows());
  Eigen::Matrix<typename DerivedV::Scalar, Eigen::Dynamic, Eigen::Dynamic> N(F.rows(),3);
  if (!uniform) {
    // compute tetrahedron face normals
    igl::per_face_normals(V,F,N); int norm_rows = N.rows();
    for (int i = 0; i < norm_rows; i++)
      N.row(i) /= N.row(i).norm();
    igl::doublearea(V,F,A); A/=2.;
  } else {
    // Use a uniform tetrahedra as a reference, with the same volume as the original one:
    //
    // Use normals of the uniform tet (V = h*[0,0,0;1,0,0;0.5,sqrt(3)/2.,0;0.5,sqrt(3)/6.,sqrt(2)/sqrt(3)])
    //         0         0    1.0000
    //         0.8165   -0.4714   -0.3333
    //         0          0.9428   -0.3333
    //         -0.8165   -0.4714   -0.3333
    for (int i = 0; i < m; i++) {
      N.row(0*m+i) << 0,0,1;
      double a = sqrt(2)*std::cbrt(3*vol(i)); // area of a face in a uniform tet with volume = vol(i)
      A(0*m+i) = (pow(a,2)*sqrt(3))/4.;
    }
    for (int i = 0; i < m; i++) {
      N.row(1*m+i) << 0.8165,-0.4714,-0.3333;
      double a = sqrt(2)*std::cbrt(3*vol(i));
      A(1*m+i) = (pow(a,2)*sqrt(3))/4.;
    }
    for (int i = 0; i < m; i++) {
      N.row(2*m+i) << 0,0.9428,-0.3333;
      double a = sqrt(2)*std::cbrt(3*vol(i));
      A(2*m+i) = (pow(a,2)*sqrt(3))/4.;
    }
    for (int i = 0; i < m; i++) {
      N.row(3*m+i) << -0.8165,-0.4714,-0.3333;
      double a = sqrt(2)*std::cbrt(3*vol(i));
      A(3*m+i) = (pow(a,2)*sqrt(3))/4.;
    }

  }

  /*  G = sparse( ...
      [0*m + repmat(1:m,1,4) ...
       1*m + repmat(1:m,1,4) ...
       2*m + repmat(1:m,1,4)], ...
      repmat([T(:,4);T(:,2);T(:,3);T(:,1)],3,1), ...
      repmat(A./(3*repmat(vol,4,1)),3,1).*N(:), ...
      3*m,n);*/
  std::vector<Triplet<double> > G_t;
  for (int i = 0; i < 4*m; i++) {
    int T_j; // j indexes : repmat([T(:,4);T(:,2);T(:,3);T(:,1)],3,1)
    switch (i/m) {
      case 0:
        T_j = 3;
        break;
      case 1:
        T_j = 1;
        break;
      case 2:
        T_j = 2;
        break;
      case 3:
        T_j = 0;
        break;
    }
    int i_idx = i%m;
    int j_idx = T(i_idx,T_j);

    double val_before_n = A(i)/(3*vol(i_idx));
    G_t.push_back(Triplet<double>(0*m+i_idx, j_idx, val_before_n * N(i,0)));
    G_t.push_back(Triplet<double>(1*m+i_idx, j_idx, val_before_n * N(i,1)));
    G_t.push_back(Triplet<double>(2*m+i_idx, j_idx, val_before_n * N(i,2)));
  }
  G.resize(3*m,n);
  G.setFromTriplets(G_t.begin(), G_t.end());
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE void grad_tri(const Eigen::PlainObjectBase<DerivedV>&V,
                     const Eigen::PlainObjectBase<DerivedF>&F,
                    Eigen::SparseMatrix<typename DerivedV::Scalar> &G,
                    bool uniform)
{
  Eigen::Matrix<typename DerivedV::Scalar,Eigen::Dynamic,3>
    eperp21(F.rows(),3), eperp13(F.rows(),3);

  for (int i=0;i<F.rows();++i)
  {
    // renaming indices of vertices of triangles for convenience
    int i1 = F(i,0);
    int i2 = F(i,1);
    int i3 = F(i,2);

    // #F x 3 matrices of triangle edge vectors, named after opposite vertices
    Eigen::Matrix<typename DerivedV::Scalar, 1, 3> v32 = V.row(i3) - V.row(i2);
    Eigen::Matrix<typename DerivedV::Scalar, 1, 3> v13 = V.row(i1) - V.row(i3);
    Eigen::Matrix<typename DerivedV::Scalar, 1, 3> v21 = V.row(i2) - V.row(i1);
    Eigen::Matrix<typename DerivedV::Scalar, 1, 3> n = v32.cross(v13);
    // area of parallelogram is twice area of triangle
    // area of parallelogram is || v1 x v2 ||
    // This does correct l2 norm of rows, so that it contains #F list of twice
    // triangle areas
    double dblA = std::sqrt(n.dot(n));
    Eigen::Matrix<typename DerivedV::Scalar, 1, 3> u(0,0,1);
    if (!uniform) {
      // now normalize normals to get unit normals
      u = n / dblA;
    } else {
      // Abstract equilateral triangle v1=(0,0), v2=(h,0), v3=(h/2, (sqrt(3)/2)*h)

      // get h (by the area of the triangle)
      double h = sqrt( (dblA)/sin(igl::PI / 3.0)); // (h^2*sin(60))/2. = Area => h = sqrt(2*Area/sin_60)

      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> v1,v2,v3;
      v1 << 0,0,0;
      v2 << h,0,0;
      v3 << h/2.,(sqrt(3)/2.)*h,0;

      // now fix v32,v13,v21 and the normal
      v32 = v3-v2;
      v13 = v1-v3;
      v21 = v2-v1;
      n = v32.cross(v13);
    }

    // rotate each vector 90 degrees around normal
    double norm21 = std::sqrt(v21.dot(v21));
    double norm13 = std::sqrt(v13.dot(v13));
    eperp21.row(i) = u.cross(v21);
    eperp21.row(i) = eperp21.row(i) / std::sqrt(eperp21.row(i).dot(eperp21.row(i)));
    eperp21.row(i) *= norm21 / dblA;
    eperp13.row(i) = u.cross(v13);
    eperp13.row(i) = eperp13.row(i) / std::sqrt(eperp13.row(i).dot(eperp13.row(i)));
    eperp13.row(i) *= norm13 / dblA;
  }

  std::vector<int> rs;
  rs.reserve(F.rows()*4*3);
  std::vector<int> cs;
  cs.reserve(F.rows()*4*3);
  std::vector<double> vs;
  vs.reserve(F.rows()*4*3);

  // row indices
  for(int r=0;r<3;r++)
  {
    for(int j=0;j<4;j++)
    {
      for(int i=r*F.rows();i<(r+1)*F.rows();i++) rs.push_back(i);
    }
  }

  // column indices
  for(int r=0;r<3;r++)
  {
    for(int i=0;i<F.rows();i++) cs.push_back(F(i,1));
    for(int i=0;i<F.rows();i++) cs.push_back(F(i,0));
    for(int i=0;i<F.rows();i++) cs.push_back(F(i,2));
    for(int i=0;i<F.rows();i++) cs.push_back(F(i,0));
  }

  // values
  for(int i=0;i<F.rows();i++) vs.push_back(eperp13(i,0));
  for(int i=0;i<F.rows();i++) vs.push_back(-eperp13(i,0));
  for(int i=0;i<F.rows();i++) vs.push_back(eperp21(i,0));
  for(int i=0;i<F.rows();i++) vs.push_back(-eperp21(i,0));
  for(int i=0;i<F.rows();i++) vs.push_back(eperp13(i,1));
  for(int i=0;i<F.rows();i++) vs.push_back(-eperp13(i,1));
  for(int i=0;i<F.rows();i++) vs.push_back(eperp21(i,1));
  for(int i=0;i<F.rows();i++) vs.push_back(-eperp21(i,1));
  for(int i=0;i<F.rows();i++) vs.push_back(eperp13(i,2));
  for(int i=0;i<F.rows();i++) vs.push_back(-eperp13(i,2));
  for(int i=0;i<F.rows();i++) vs.push_back(eperp21(i,2));
  for(int i=0;i<F.rows();i++) vs.push_back(-eperp21(i,2));

  // create sparse gradient operator matrix
  G.resize(3*F.rows(),V.rows());
  std::vector<Eigen::Triplet<typename DerivedV::Scalar> > triplets;
  for (int i=0;i<(int)vs.size();++i)
  {
    triplets.push_back(Eigen::Triplet<typename DerivedV::Scalar>(rs[i],cs[i],vs[i]));
  }
  G.setFromTriplets(triplets.begin(), triplets.end());
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::grad(const Eigen::PlainObjectBase<DerivedV>&V,
                     const Eigen::PlainObjectBase<DerivedF>&F,
                    Eigen::SparseMatrix<typename DerivedV::Scalar> &G,
                    bool uniform)
{
  assert(F.cols() == 3 || F.cols() == 4);
  if (F.cols() == 3)
    return grad_tri(V,F,G,uniform);
  if (F.cols() == 4)
    return grad_tet(V,F,G,uniform);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::grad<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, 0, int>&, bool);
template void igl::grad<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::SparseMatrix<Eigen::Matrix<double, -1, 3, 0, -1, 3>::Scalar, 0, int>&, bool);
#endif
