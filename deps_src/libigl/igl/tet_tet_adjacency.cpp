// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Oded Stein <oded.stein@columbia.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.


#include "tet_tet_adjacency.h"

#include "parallel_for.h"

#include <array>
#include <vector>

template <typename DerivedT, typename DerivedTT, typename DerivedTTi>
IGL_INLINE void
igl::tet_tet_adjacency(
                       const Eigen::MatrixBase<DerivedT>& T,
                       Eigen::PlainObjectBase<DerivedTT>& TT,
                       Eigen::PlainObjectBase<DerivedTTi>& TTi)
{
  assert(T.cols()==4 && "Tets have four vertices.");
  
  //Preprocess
  using Array = std::array<typename DerivedT::Scalar, 5>;
  std::vector<Array> TTT(4*T.rows());
  const auto loop_f = [&](const int t) {
    TTT[4*t] = {T(t,0),T(t,1),T(t,2),t,0};
    TTT[4*t+1] = {T(t,0),T(t,1),T(t,3),t,1};
    TTT[4*t+2] = {T(t,1),T(t,2),T(t,3),t,2};
    TTT[4*t+3] = {T(t,2),T(t,0),T(t,3),t,3};
    for(int i=0; i<4; ++i)
      std::sort(TTT[4*t+i].begin(), TTT[4*t+i].begin()+3);
  };
  
  //for(int t=0; t<T.rows(); ++t)
  //    loop_f(t);
  igl::parallel_for(T.rows(), loop_f);
  
  std::sort(TTT.begin(),TTT.end());
  
  //Compute TT and TTi
  TT.setConstant(T.rows(), T.cols(), -1);
  TTi.setConstant(T.rows(), T.cols(), -1);
  for(int i=1; i<TTT.size(); ++i) {
    const Array& r1 = TTT[i-1];
    const Array& r2 = TTT[i];
    if((r1[0]==r2[0]) && (r1[1]==r2[1]) && (r1[2]==r2[2])) {
      TT(r1[3],r1[4]) = r2[3];
      TT(r2[3],r2[4]) = r1[3];
      TTi(r1[3],r1[4]) = r2[4];
      TTi(r2[3],r2[4]) = r1[4];
    }
  }
}


template <typename DerivedT, typename DerivedTT>
IGL_INLINE void
igl::tet_tet_adjacency(
                       const Eigen::MatrixBase<DerivedT>& T,
                       Eigen::PlainObjectBase<DerivedTT>& TT)
{
  DerivedTT TTi;
  tet_tet_adjacency(T, TT, TTi);
}

#ifdef IGL_STATIC_LIBRARY
template void igl::tet_tet_adjacency<Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif

