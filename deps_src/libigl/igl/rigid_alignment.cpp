// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2019 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "rigid_alignment.h"
#include "PlainMatrix.h"
#include <Eigen/Sparse>
#include <Eigen/QR>
// Not currently used. See below.
//#include <Eigen/Cholesky>
#include <vector>
#include <iostream>

template <
  typename DerivedX,
  typename DerivedP,
  typename DerivedN,
  typename DerivedR,
  typename Derivedt
>
IGL_INLINE void igl::rigid_alignment(
  const Eigen::MatrixBase<DerivedX> & _X,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedN> & N,
  Eigen::PlainObjectBase<DerivedR> & R,
  Eigen::PlainObjectBase<Derivedt> & t)
{
  typedef typename DerivedX::Scalar Scalar;
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,Eigen::Dynamic> MatrixXS;
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,1> VectorXS;
  typedef Eigen::Matrix<Scalar,3,3> Matrix3S;
  const int k = _X.rows();
  VectorXS Z = VectorXS::Zero(k,1);
  VectorXS I = VectorXS::Ones(k,1);

  PlainMatrix<DerivedX> X = _X;
  R = DerivedR::Identity(3,3);
  t = Derivedt::Zero(1,3);
  // See gptoolbox, each iter could be O(1) instead of O(k)
  const int max_iters = 5;
  // Weight on point-to-point regularization.
  Scalar w = 1e-5;
  for(int iters = 0;iters<max_iters;iters++)
  {
    MatrixXS A(k*3,6);
    A <<
               Z, X.col(2),-X.col(1),I,Z,Z,
       -X.col(2),        Z, X.col(0),Z,I,Z,
        X.col(1),-X.col(0),        Z,Z,Z,I;
    VectorXS B(k*3,1);
    B<<
      P.col(0)-X.col(0),
      P.col(1)-X.col(1),
      P.col(2)-X.col(2);


    std::vector<Eigen::Triplet<Scalar> > NNIJV;
    for(int i = 0;i<k;i++)
    {
      for(int c = 0;c<3;c++)
      {
        NNIJV.emplace_back(i,i+k*c,N(i,c));
      }
    }
    Eigen::SparseMatrix<Scalar> NN(k,k*3);
    NN.setFromTriplets(NNIJV.begin(),NNIJV.end());

    MatrixXS NA = (NN * A).eval();
    VectorXS NB = (NN * B).eval();

    MatrixXS Q = (1.0-w)*(NA.transpose() * NA) + w * A.transpose() * A;
    VectorXS f = (1.0-w)*(NA.transpose() * NB) + w * A.transpose() * B;
    // This could be a lot faster but isn't rank revealing and may produce wonky
    // results when P and N are all the same point and vector.
    //VectorXS u = (Q).ldlt().solve(f);
    
    Eigen::CompleteOrthogonalDecomposition<decltype(Q)> qr(Q);
    if(qr.rank() < 6)
    {
      w = 1.0-(1.0-w)/2.0;
    }

    VectorXS u = qr.solve(f);
    Derivedt ti = u.tail(3).transpose();
    Matrix3S W;
    W<<
          0, u(2),-u(1),
      -u(2),    0, u(0),
       u(1),-u(0),    0;
    // strayed from a perfect rotation. Correct it.
    const double x = u.head(3).stableNorm();
    DerivedR Ri;
    if(x == 0)
    {
      Ri = DerivedR::Identity(3,3);
    }else
    {
      Ri = 
        DerivedR::Identity(3,3) + 
        sin(x)/x*W + 
        (1.0-cos(x))/(x*x)*W*W;
    }
    
    R = (R*Ri).eval();
    t = (t*Ri + ti).eval();
    X = ((_X*R).rowwise()+t).eval();
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::rigid_alignment<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 3, 3, 0, 3, 3>, Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 3, 0, 3, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&);
#endif
