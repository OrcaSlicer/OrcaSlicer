// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "upsample.h"

#include "triangle_triangle_adjacency.h"


template <
  typename DerivedF,
  typename SType,
  typename DerivedNF>
IGL_INLINE void igl::upsample(
  const int n_verts,
  const Eigen::PlainObjectBase<DerivedF>& F,
  Eigen::SparseMatrix<SType>& S,
  Eigen::PlainObjectBase<DerivedNF>& NF)
{
  using namespace std;
  using namespace Eigen;

  typedef Eigen::Triplet<SType> Triplet_t;

  Eigen::Matrix< typename DerivedF::Scalar,Eigen::Dynamic,Eigen::Dynamic>
    FF,FFi;
  triangle_triangle_adjacency(F,FF,FFi);

  // TODO: Cache optimization missing from here, it is a mess

  // Compute the number and positions of the vertices to insert (on edges)
  Eigen::MatrixXi NI = Eigen::MatrixXi::Constant(FF.rows(),FF.cols(),-1);
  Eigen::MatrixXi NIdoubles = Eigen::MatrixXi::Zero(FF.rows(), FF.cols());
  int counter = 0;

  for(int i=0;i<FF.rows();++i)
  {
    for(int j=0;j<3;++j)
    {
      if(NI(i,j) == -1)
      {
        NI(i,j) = counter;
        NIdoubles(i,j) = 0;
        if (FF(i,j) != -1) {
          //If it is not a boundary
          NI(FF(i,j), FFi(i,j)) = counter;
          NIdoubles(i,j) = 1;
        }
        ++counter;
      }
    }
  }

  const int& n_odd = n_verts;
  const int& n_even = counter;
  const int n_newverts = n_odd + n_even;

  //Construct vertex positions
  std::vector<Triplet_t> tripletList;

  // Fill the odd vertices position
  for (int i=0; i<n_odd; ++i)
  {
    tripletList.emplace_back(i, i, 1.);
  }

  for(int i=0;i<FF.rows();++i)
  {
    for(int j=0;j<3;++j)
    {
      if(NIdoubles(i,j)==0) {
        tripletList.emplace_back(NI(i,j) + n_odd, F(i,j), 1./2.);
        tripletList.emplace_back(NI(i,j) + n_odd, F(i,(j+1)%3), 1./2.);
      }
    }
  }
  S.resize(n_newverts, n_verts);
  S.setFromTriplets(tripletList.begin(), tripletList.end());

  // Build the new topology (Every face is replaced by four)
  NF.resize(F.rows()*4,3);
  for(int i=0; i<F.rows();++i)
  {
    VectorXi VI(6);
    VI << F(i,0), F(i,1), F(i,2), NI(i,0) + n_odd, NI(i,1) + n_odd, NI(i,2) + n_odd;

    VectorXi f0(3), f1(3), f2(3), f3(3);
    f0 << VI(0), VI(3), VI(5);
    f1 << VI(1), VI(4), VI(3);
    f2 << VI(3), VI(4), VI(5);
    f3 << VI(4), VI(2), VI(5);

    NF.row((i*4)+0) = f0;
    NF.row((i*4)+1) = f1;
    NF.row((i*4)+2) = f2;
    NF.row((i*4)+3) = f3;
  }
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedNV,
  typename DerivedNF>
IGL_INLINE void igl::upsample(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  Eigen::PlainObjectBase<DerivedNV>& NV,
  Eigen::PlainObjectBase<DerivedNF>& NF,
  const int number_of_subdivs)
{
  NV = V;
  NF = F;
  for(int i=0; i<number_of_subdivs; ++i) 
  {
    DerivedNF tempF = NF;
    Eigen::SparseMatrix<typename DerivedV::Scalar >S;
    upsample(NV.rows(), tempF, S, NF);
    // This .eval is super important
    NV = (S*NV).eval();
  }
}

template <
  typename MatV,
  typename MatF>
IGL_INLINE void igl::upsample(
  MatV& V,
  MatF& F,
  const int number_of_subdivs)
{
  const MatV V_copy = V;
  const MatF F_copy = F;
  return upsample(V_copy,F_copy,V,F,number_of_subdivs);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::upsample<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, int);
template void igl::upsample<Eigen::Matrix<int, -1, -1, 0, -1, -1>, double, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::SparseMatrix<double, 0, int>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::upsample<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::Matrix<double, -1, -1, 0, -1, -1>&, Eigen::Matrix<int, -1, -1, 0, -1, -1>&, int);
#endif
