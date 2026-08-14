// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "ismember.h"
#include "colon.h"
#include "list_to_matrix.h"
#include "sort.h"
#include "sortrows.h"
#include "unique.h"
#include "unique_rows.h"
#include <iostream>

template <
  typename DerivedA,
  typename DerivedB,
  typename DerivedIA,
  typename DerivedLOCB>
IGL_INLINE void igl::ismember(
  const Eigen::MatrixBase<DerivedA> & A,
  const Eigen::MatrixBase<DerivedB> & B,
  Eigen::PlainObjectBase<DerivedIA> & IA,
  Eigen::PlainObjectBase<DerivedLOCB> & LOCB)
{
  using namespace Eigen;
  using namespace std;
  IA.resizeLike(A);
  IA.setConstant(false);
  LOCB.resizeLike(A);
  LOCB.setConstant(-1);
  // boring base cases
  if(A.size() == 0)
  {
    return;
  }
  if(B.size() == 0)
  {
    return;
  }

  // Get rid of any duplicates
  typedef Matrix<typename DerivedA::Scalar,Dynamic,1> VectorA;
  typedef Matrix<typename DerivedB::Scalar,Dynamic,1> VectorB;
  const VectorA vA(Eigen::Map<const VectorA>(DerivedA(A).data(), A.cols()*A.rows(),1));
  const VectorB vB(Eigen::Map<const VectorB>(DerivedB(B).data(), B.cols()*B.rows(),1));
  VectorA uA;
  VectorB uB;
  Eigen::Matrix<typename DerivedA::Index,Dynamic,1> uIA,uIuA,uIB,uIuB;
  unique(vA,uA,uIA,uIuA);
  unique(vB,uB,uIB,uIuB);
  // Sort both
  VectorA sA;
  VectorB sB;
  Eigen::Matrix<typename DerivedA::Index,Dynamic,1> sIA,sIB;
  sort(uA,1,true,sA,sIA);
  sort(uB,1,true,sB,sIB);

  Eigen::Matrix<bool,Eigen::Dynamic,1> uF = 
    Eigen::Matrix<bool,Eigen::Dynamic,1>::Zero(sA.size(),1);
  Eigen::Matrix<typename DerivedLOCB::Scalar, Eigen::Dynamic,1> uLOCB =
    Eigen::Matrix<typename DerivedLOCB::Scalar,Eigen::Dynamic,1>::
    Constant(sA.size(),1,-1);
  {
    int bi = 0;
    // loop over sA
    bool past = false;
    for(int a = 0;a<sA.size();a++)
    {
      while(!past && sA(a)>sB(bi))
      {
        bi++;
        past = bi>=sB.size();
      }
      if(!past && sA(a)==sB(bi))
      {
        uF(sIA(a)) = true;
        uLOCB(sIA(a)) = uIB(sIB(bi));
      }
    }
  }

  Map< Matrix<typename DerivedIA::Scalar,Dynamic,1> > 
    vIA(IA.data(),IA.cols()*IA.rows(),1);
  Map< Matrix<typename DerivedLOCB::Scalar,Dynamic,1> > 
    vLOCB(LOCB.data(),LOCB.cols()*LOCB.rows(),1);
  for(int a = 0;a<A.size();a++)
  {
    vIA(a) = uF(uIuA(a));
    vLOCB(a) = uLOCB(uIuA(a));
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::ismember<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<bool, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<bool, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
