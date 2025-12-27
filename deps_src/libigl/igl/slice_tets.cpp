// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "slice_tets.h"
#include "LinSpaced.h"
#include "sort.h"
#include "edges.h"
#include "slice.h"
#include "cat.h"
#include "ismember.h"
#include "unique_rows.h"
#include <cassert>
#include <algorithm>
#include <vector>

template <
  typename DerivedV, 
  typename DerivedT, 
  typename DerivedS,
  typename DerivedSV,
  typename DerivedSF,
  typename DerivedJ,
  typename BCType>
IGL_INLINE void igl::slice_tets(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedT>& T,
  const Eigen::MatrixBase<DerivedS> & S,
  Eigen::PlainObjectBase<DerivedSV>& SV,
  Eigen::PlainObjectBase<DerivedSF>& SF,
  Eigen::PlainObjectBase<DerivedJ>& J,
  Eigen::SparseMatrix<BCType> & BC)
{
  Eigen::MatrixXi sE;
  Eigen::Matrix<typename DerivedSV::Scalar,Eigen::Dynamic,1> lambda;
  igl::slice_tets(V,T,S,SV,SF,J,sE,lambda);
  const int ns = SV.rows();
  std::vector<Eigen::Triplet<BCType> > BCIJV(ns*2);
  for(int i = 0;i<ns;i++)
  {
    BCIJV[2*i+0] = Eigen::Triplet<BCType>(i,sE(i,0),    lambda(i));
    BCIJV[2*i+1] = Eigen::Triplet<BCType>(i,sE(i,1),1.0-lambda(i));
  }
  BC.resize(SV.rows(),V.rows());
  BC.setFromTriplets(BCIJV.begin(),BCIJV.end());
}

template <
  typename DerivedV, 
  typename DerivedT, 
  typename DerivedS,
  typename DerivedSV,
  typename DerivedSF,
  typename DerivedJ>
IGL_INLINE void igl::slice_tets(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedT>& T,
  const Eigen::MatrixBase<DerivedS> & S,
  Eigen::PlainObjectBase<DerivedSV>& SV,
  Eigen::PlainObjectBase<DerivedSF>& SF,
  Eigen::PlainObjectBase<DerivedJ>& J)
{
  Eigen::MatrixXi sE;
  Eigen::Matrix<typename DerivedSV::Scalar,Eigen::Dynamic,1> lambda;
  igl::slice_tets(V,T,S,SV,SF,J,sE,lambda);
}

template <
  typename DerivedV, 
  typename DerivedT, 
  typename DerivedS,
  typename DerivedSV,
  typename DerivedSF,
  typename DerivedJ,
  typename DerivedsE,
  typename Derivedlambda>
IGL_INLINE void igl::slice_tets(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<DerivedT>& T,
  const Eigen::MatrixBase<DerivedS> & S,
  Eigen::PlainObjectBase<DerivedSV>& SV,
  Eigen::PlainObjectBase<DerivedSF>& SF,
  Eigen::PlainObjectBase<DerivedJ>& J,
  Eigen::PlainObjectBase<DerivedsE>& sE,
  Eigen::PlainObjectBase<Derivedlambda>& lambda)
{

  using namespace Eigen;
  using namespace std;
  assert(V.cols() == 3 && "V should be #V by 3");
  assert(T.cols() == 4 && "T should be #T by 4");

  static const Eigen::Matrix<int,12,4> flipped_order = 
    (Eigen::Matrix<int,12,4>(12,4)<<
      3,2,0,1,
      3,1,2,0,
      3,0,1,2,
      2,3,1,0,
      2,1,0,3,
      2,0,3,1,
      1,3,0,2,
      1,2,3,0,
      1,0,2,3,
      0,3,2,1,
      0,2,1,3,
      0,1,3,2
    ).finished();

  // number of tets
  const size_t m = T.rows();

  typedef typename DerivedS::Scalar Scalar;
  typedef typename DerivedT::Scalar Index;
  typedef Matrix<Scalar,Dynamic,1> VectorXS;
  typedef Matrix<Scalar,Dynamic,4> MatrixX4S;
  typedef Matrix<Scalar,Dynamic,3> MatrixX3S;
  typedef Matrix<Scalar,Dynamic,2> MatrixX2S;
  typedef Matrix<Index,Dynamic,4> MatrixX4I;
  typedef Matrix<Index,Dynamic,3> MatrixX3I;
  typedef Matrix<Index,Dynamic,2> MatrixX2I;
  typedef Matrix<Index,Dynamic,1> VectorXI;
  typedef Array<bool,Dynamic,1> ArrayXb;
  
  MatrixX4S IT(m,4);
  for(size_t t = 0;t<m;t++)
  {
    for(size_t c = 0;c<4;c++)
    {
      IT(t,c) = S(T(t,c));
    }
  }

  // Essentially, just a glorified slice(X,1)
  // 
  // Inputs:
  //   T  #T by 4 list of tet indices into V
  //   IT  #IT by 4 list of isosurface values at each tet
  //   I  #I list of bools whether to grab data corresponding to each tet
  const auto & extract_rows = [](
    const MatrixBase<DerivedT> & T,
    const MatrixX4S & IT,
    const ArrayXb & I,
    MatrixX4I  & TI,
    MatrixX4S & ITI,
    VectorXI & JI)
  {
    const Index num_I = std::count(I.data(),I.data()+I.size(),true);
    TI.resize(num_I,4);
    ITI.resize(num_I,4);
    JI.resize(num_I,1);
    {
      size_t k = 0;
      for(size_t t = 0;t<(size_t)T.rows();t++)
      {
        if(I(t))
        {
          TI.row(k) = T.row(t);
          ITI.row(k) = IT.row(t);
          JI(k) = t;
          k++;
        }
      }
      assert(k == num_I);
    }
  };

  ArrayXb I13 = (IT.array()<0).rowwise().count()==1;
  ArrayXb I31 = (IT.array()>0).rowwise().count()==1;
  ArrayXb I22 = (IT.array()<0).rowwise().count()==2;
  MatrixX4I T13,T31,T22;
  MatrixX4S IT13,IT31,IT22;
  VectorXI J13,J31,J22;
  extract_rows(T,IT,I13,T13,IT13,J13);
  extract_rows(T,IT,I31,T31,IT31,J31);
  extract_rows(T,IT,I22,T22,IT22,J22);

  const auto & apply_sort4 = [] (
     const MatrixX4I & T, 
     const MatrixX4I & sJ, 
     MatrixX4I & sT)
  {
    sT.resize(T.rows(),4);
    for(size_t t = 0;t<(size_t)T.rows();t++)
    {
      for(size_t c = 0;c<4;c++)
      {
        sT(t,c) = T(t,sJ(t,c));
      }
    }
  };

  const auto & apply_sort2 = [] (
     const MatrixX2I & E, 
     const MatrixX2I & sJ, 
     Eigen::PlainObjectBase<DerivedsE>& sE)
  {
    sE.resize(E.rows(),2);
    for(size_t t = 0;t<(size_t)E.rows();t++)
    {
      for(size_t c = 0;c<2;c++)
      {
        sE(t,c) = E(t,sJ(t,c));
      }
    }
  };

  const auto & one_below = [&apply_sort4](
    const MatrixX4I & T,
    const MatrixX4S & IT,
    MatrixX2I & U,
    MatrixX3I & SF)
  {
    // Number of tets
    const size_t m = T.rows();
    if(m == 0)
    {
      U.resize(0,2);
      SF.resize(0,3);
      return;
    }
    MatrixX4S sIT;
    MatrixX4I sJ;
    sort(IT,2,true,sIT,sJ);
    MatrixX4I sT;
    apply_sort4(T,sJ,sT);
    U.resize(3*m,2);
    U<<
      sT.col(0),sT.col(1),
      sT.col(0),sT.col(2),
      sT.col(0),sT.col(3);
    SF.resize(m,3);
    for(size_t c = 0;c<3;c++)
    {
      SF.col(c) = 
        igl::LinSpaced<
        Eigen::Matrix<typename DerivedSF::Scalar,Eigen::Dynamic,1> >
        (m,0+c*m,(m-1)+c*m);
    }
    ArrayXb flip;
    {
      VectorXi _;
      ismember_rows(sJ,flipped_order,flip,_);
    }
    for(int i = 0;i<m;i++)
    {
      if(flip(i))
      {
        SF.row(i) = SF.row(i).reverse().eval();
      }
    }
  };

  const auto & two_below = [&apply_sort4](
    const MatrixX4I & T,
    const MatrixX4S & IT,
    MatrixX2I & U,
    MatrixX3I & SF)
  {
    // Number of tets
    const size_t m = T.rows();
    if(m == 0)
    {
      U.resize(0,2);
      SF.resize(0,3);
      return;
    }
    MatrixX4S sIT;
    MatrixX4I sJ;
    sort(IT,2,true,sIT,sJ);
    MatrixX4I sT;
    apply_sort4(T,sJ,sT);
    U.resize(4*m,2);
    U<<
      sT.col(0),sT.col(2),
      sT.col(0),sT.col(3),
      sT.col(1),sT.col(2),
      sT.col(1),sT.col(3);
    SF.resize(2*m,3);
    SF.block(0,0,m,1) = igl::LinSpaced<VectorXI >(m,0+0*m,(m-1)+0*m);
    SF.block(0,1,m,1) = igl::LinSpaced<VectorXI >(m,0+1*m,(m-1)+1*m);
    SF.block(0,2,m,1) = igl::LinSpaced<VectorXI >(m,0+3*m,(m-1)+3*m);
    SF.block(m,0,m,1) = igl::LinSpaced<VectorXI >(m,0+0*m,(m-1)+0*m);
    SF.block(m,1,m,1) = igl::LinSpaced<VectorXI >(m,0+3*m,(m-1)+3*m);
    SF.block(m,2,m,1) = igl::LinSpaced<VectorXI >(m,0+2*m,(m-1)+2*m);
    ArrayXb flip;
    {
      VectorXi _;
      ismember_rows(sJ,flipped_order,flip,_);
    }
    for(int i = 0;i<m;i++)
    {
      if(flip(i))
      {
        SF.row(i  ) = SF.row(i  ).reverse().eval();
        SF.row(i+m) = SF.row(i+m).reverse().eval();
      }
    }
  };

  MatrixX3I SF13,SF31,SF22;
  MatrixX2I U13,U31,U22;
  one_below(T13, IT13,U13,SF13);
  one_below(T31,-IT31,U31,SF31);
  two_below(T22, IT22,U22,SF22);
  // https://forum.kde.org/viewtopic.php?f=74&t=107974
  const MatrixX2I U = 
    (MatrixX2I(U13.rows()+ U31.rows()+ U22.rows(),2)<<U13,U31,U22).finished();
  MatrixX2I sU;
  {
    MatrixX2I _;
    sort(U,2,true,sU,_);
  }
  MatrixX2I E;
  VectorXI uI,uJ;
  unique_rows(sU,E,uI,uJ);
  MatrixX2S IE(E.rows(),2);
  for(size_t t = 0;t<E.rows();t++)
  {
    for(size_t c = 0;c<2;c++)
    {
      IE(t,c) = S(E(t,c));
    }
  }
  MatrixX2S sIE;
  MatrixX2I sJ;
  sort(IE,2,true,sIE,sJ);
  apply_sort2(E,sJ,sE);
  lambda = sIE.col(1).array() / (sIE.col(1)-sIE.col(0)).array();
  SV.resize(sE.rows(),V.cols());
  for(int e = 0;e<sE.rows();e++)
  {
    SV.row(e) = V.row(sE(e,0)).template cast<Scalar>()*lambda(e) + 
                V.row(sE(e,1)).template cast<Scalar>()*(1.0-lambda(e));
  }
  SF.resize( SF13.rows()+SF31.rows()+SF22.rows(),3);
  SF<<
    SF13,
    U13.rows()+           SF31.rowwise().reverse().array(),
    U13.rows()+U31.rows()+SF22.array();

  std::for_each(
    SF.data(),
    SF.data()+SF.size(),
    [&uJ](typename DerivedSF::Scalar & i){i=uJ(i);});

  J.resize(SF.rows());
  J<<J13,J31,J22,J22;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::slice_tets<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, double>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::SparseMatrix<double, 0, int>&);
#endif
