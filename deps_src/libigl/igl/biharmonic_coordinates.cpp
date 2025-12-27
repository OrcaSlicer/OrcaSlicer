// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "biharmonic_coordinates.h"
#include "cotmatrix.h"
#include "sum.h"
#include "massmatrix.h"
#include "min_quad_with_fixed.h"
#include "crouzeix_raviart_massmatrix.h"
#include "crouzeix_raviart_cotmatrix.h"
#include "normal_derivative.h"
#include "on_boundary.h"
#include <Eigen/Sparse>

template <
  typename DerivedV,
  typename DerivedT,
  typename SType,
  typename DerivedW>
IGL_INLINE bool igl::biharmonic_coordinates(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedT> & T,
  const std::vector<std::vector<SType> > & S,
  Eigen::PlainObjectBase<DerivedW> & W)
{
  return biharmonic_coordinates(V,T,S,2,W);
}

template <
  typename DerivedV,
  typename DerivedT,
  typename SType,
  typename DerivedW>
IGL_INLINE bool igl::biharmonic_coordinates(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedT> & T,
  const std::vector<std::vector<SType> > & S,
  const int k,
  Eigen::PlainObjectBase<DerivedW> & W)
{
  using namespace Eigen;
  using namespace std;
  // This is not the most efficient way to build A, but follows "Linear
  // Subspace Design for Real-Time Shape Deformation" [Wang et al. 2015].
  SparseMatrix<double> A;
  {
    DiagonalMatrix<double,Dynamic> Minv;
    SparseMatrix<double> L,K;
    Array<bool,Dynamic,Dynamic> C;
    {
      Array<bool,Dynamic,1> I;
      on_boundary(T,I,C);
    }
#ifdef false 
    // Version described in paper is "wrong"
    // http://www.cs.toronto.edu/~jacobson/images/error-in-linear-subspace-design-for-real-time-shape-deformation-2017-wang-et-al.pdf
    SparseMatrix<double> N,Z,M;
    normal_derivative(V,T,N);
    {
      std::vector<Triplet<double> >ZIJV;
      for(int t =0;t<T.rows();t++)
      {
        for(int f =0;f<T.cols();f++)
        {
          if(C(t,f))
          {
            const int i = t+f*T.rows();
            for(int c = 1;c<T.cols();c++)
            {
              ZIJV.emplace_back(T(t,(f+c)%T.cols()),i,1);
            }
          }
        }
      }
      Z.resize(V.rows(),N.rows());
      Z.setFromTriplets(ZIJV.begin(),ZIJV.end());
      N = (Z*N).eval();
    }
    cotmatrix(V,T,L);
    K = N+L;
    massmatrix(V,T,MASSMATRIX_TYPE_DEFAULT,M);
    // normalize
    M /= ((VectorXd)M.diagonal()).array().abs().maxCoeff();
    Minv =
      ((VectorXd)M.diagonal().array().inverse()).asDiagonal();
#else
    Eigen::SparseMatrix<double> M;
    Eigen::MatrixXi E;
    Eigen::VectorXi EMAP;
    crouzeix_raviart_massmatrix(V,T,M,E,EMAP);
    crouzeix_raviart_cotmatrix(V,T,E,EMAP,L);
    // Ad  #E by #V facet-vertex incidence matrix
    Eigen::SparseMatrix<double> Ad(E.rows(),V.rows());
    {
      std::vector<Eigen::Triplet<double> > AIJV(E.size());
      for(int e = 0;e<E.rows();e++)
      {
        for(int c = 0;c<E.cols();c++)
        {
          AIJV[e+c*E.rows()] = Eigen::Triplet<double>(e,E(e,c),1);
        }
      }
      Ad.setFromTriplets(AIJV.begin(),AIJV.end());
    }
    // Degrees
    Eigen::VectorXd De;
    sum(Ad,2,De);
    Eigen::DiagonalMatrix<double,Eigen::Dynamic> De_diag = 
      De.array().inverse().matrix().asDiagonal();
    K = L*(De_diag*Ad);
    // normalize
    M /= ((VectorXd)M.diagonal()).array().abs().maxCoeff();
    Minv = ((VectorXd)M.diagonal().array().inverse()).asDiagonal();
    // kill boundary edges
    for(int f = 0;f<T.rows();f++)
    {
      for(int c = 0;c<T.cols();c++)
      {
        if(C(f,c))
        {
          const int e = EMAP(f+T.rows()*c);
          Minv.diagonal()(e) = 0;
        }
      }
    }

#endif
    switch(k)
    {
      default:
        assert(false && "unsupported");
      case 2:
        // For C1 smoothness in 2D, one should use bi-harmonic
        A = K.transpose() * (Minv * K);
        break;
      case 3:
        // For C1 smoothness in 3D, one should use tri-harmonic
        A = K.transpose() * (Minv * (-L * (Minv * K)));
        break;
    }
  }
  // Vertices in point handles
  const size_t mp =
    count_if(S.begin(),S.end(),[](const vector<int> & h){return h.size()==1;});
  // number of region handles
  const size_t r = S.size()-mp;
  // Vertices in region handles
  size_t mr = 0;
  for(const auto & h : S)
  {
    if(h.size() > 1)
    {
      mr += h.size();
    }
  }
  const size_t dim = T.cols()-1;
  // Might as well be dense... I think...
  MatrixXd J = MatrixXd::Zero(mp+mr,mp+r*(dim+1));
  VectorXi b(mp+mr);
  MatrixXd H(mp+r*(dim+1),dim);
  {
    int v = 0;
    int c = 0;
    for(int h = 0;h<S.size();h++)
    {
      if(S[h].size()==1)
      {
        H.row(c) = V.block(S[h][0],0,1,dim);
        J(v,c++) = 1;
        b(v) = S[h][0];
        v++;
      }else
      {
        assert(S[h].size() >= dim+1);
        for(int p = 0;p<S[h].size();p++)
        {
          for(int d = 0;d<dim;d++)
          {
            J(v,c+d) = V(S[h][p],d);
          }
          J(v,c+dim) = 1;
          b(v) = S[h][p];
          v++;
        }
        H.block(c,0,dim+1,dim).setIdentity();
        c+=dim+1;
      }
    }
  }
  // minimize    ½ W' A W'
  // subject to  W(b,:) = J
  return min_quad_with_fixed(
    A,VectorXd::Zero(A.rows()).eval(),b,J,SparseMatrix<double>(),VectorXd(),true,W);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::biharmonic_coordinates<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, int, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
