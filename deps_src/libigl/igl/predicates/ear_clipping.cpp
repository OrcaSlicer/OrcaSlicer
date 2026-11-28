// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2019 Hanxiao Shen <hanxiao@cs.nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "ear_clipping.h"
#include "predicates.h"
#include "segment_segment_intersect.h"
#include "../turning_number.h"

template <
  typename DerivedP, 
  typename DerivedRT,
  typename DerivedF, 
  typename DerivedI>
IGL_INLINE void igl::predicates::ear_clipping(
  const Eigen::MatrixBase<DerivedP>& P,
  const Eigen::MatrixBase<DerivedRT>& RT,
  Eigen::PlainObjectBase<DerivedF>& eF, 
  Eigen::PlainObjectBase<DerivedI>& I)
{
  typedef typename DerivedF::Scalar Index;
  typedef typename DerivedP::Scalar Scalar;
  static_assert(std::is_same<typename DerivedI::Scalar,
                             typename DerivedF::Scalar>::value,
                "index type should be consistent");
  
  // check whether vertex i is an ear
  auto is_ear = [](
    const Eigen::MatrixBase<DerivedP>& P,
    const Eigen::MatrixBase<DerivedRT>& RT,
    const Eigen::Matrix<Index,Eigen::Dynamic,1>& L,
    const Eigen::Matrix<Index,Eigen::Dynamic,1>& R,
    const Index i
  ){
    
    // check if any edge in the leftover polygon intersects triangle (a,b,i)

    Index a = L(i), b = R(i);
    if(RT(i) != 0 || RT(a) != 0 || RT(b) != 0) return false;
    Eigen::Matrix<Scalar,1,2> pa = P.row(a);
    Eigen::Matrix<Scalar,1,2> pb = P.row(b);
    Eigen::Matrix<Scalar,1,2> pi = P.row(i);
    auto r = igl::predicates::orient2d(pa, pi, pb);
    if(r == igl::predicates::Orientation::NEGATIVE ||
       r == igl::predicates::Orientation::COLLINEAR) return false;

    // check edge (b, R(b))
    Index k = b;
    Index l = R(b);
    Eigen::Matrix<Scalar, 1, 2> pk = P.row(k);
    Eigen::Matrix<Scalar, 1, 2> pl = P.row(l);
    auto r1 = igl::predicates::orient2d(pb, pl, pa);
    auto r2 = igl::predicates::orient2d(pi, pl, pb);
    if (l == a) return true;  // single triangle
    if (r1 != igl::predicates::Orientation::POSITIVE &&
        r2 != igl::predicates::Orientation::POSITIVE) {
      return false;
    }
    
    // iterate through all edges do not have common endpoints with a, b
    k = l;
    l = R(k);
    while (l != a) {
      pk = P.row(k);
      pl = P.row(l);
      if (igl::predicates::segment_segment_intersect(pa, pb, pk, pl) ||
          igl::predicates::segment_segment_intersect(pa, pi, pk, pl) ||
          igl::predicates::segment_segment_intersect(pi, pb, pk, pl)
      ){
        return false;
      }
      k = l;
      l = R(k);
    }

    // check edge (L(a), a)
    pk = P.row(k);
    pl = P.row(l);
    r1 = igl::predicates::orient2d(pb, pk, pl);
    r2 = igl::predicates::orient2d(pi, pa, pk);
    if (r1 != igl::predicates::Orientation::POSITIVE &&
        r2 != igl::predicates::Orientation::POSITIVE
    ){
      return false;
    }

    return true;
  };

  Eigen::Matrix<Index,Eigen::Dynamic,1> L(P.rows());
  Eigen::Matrix<Index,Eigen::Dynamic,1> R(P.rows());
  for(int i = 0;i < P.rows(); i++){
    L(i) = Index((i - 1 + P.rows()) % P.rows());
    R(i) = Index((i + 1) % P.rows());
  }

  Eigen::Matrix<Index, Eigen::Dynamic, 1> ears; // mark ears
  Eigen::Matrix<Index, Eigen::Dynamic, 1> X; // clipped vertices
  ears.setZero(P.rows());
  X.setZero(P.rows());

  // initialize ears
  for(int i = 0; i < P.rows(); i++){
    ears(i) = is_ear(P, RT, L, R, i);
  }

  // clip ears until none left
  while(ears.maxCoeff()==1){
    
    // find the first ear
    Index e = 0;
    while(e < ears.rows() && ears(e) != 1) e++;
    
    // find valid neighbors
    Index a = L(e), b = R(e);
    if(a == b) break;

    // clip ear and store face
    eF.conservativeResize(eF.rows()+1,3);
    eF.bottomRows(1)<<a, e, b;
    L(b) = a;
    L(e) = -1;
    R(a) = b;
    R(e) = -1;
    ears(e) = 0; // mark vertex e as non-ear

    // update neighbor's ear status
    ears(a) = is_ear(P, RT, L, R, a);
    ears(b) = is_ear(P, RT, L, R, b);
    X(e) = 1;

    // when only one edge left
    // mark the endpoints as clipped
    if(L(a) == b && R(b) == a){
      X(a) = 1;
      X(b) = 1;
    }
  }
  
  // collect remaining vertices if theres any
  for(int i = 0;i < X.rows(); i++)
  {
    X(i) = 1 - X(i);
  }
  I.resize(X.sum());
  Index j=0;
  for(Index i = 0;i < X.rows(); i++)
  {
    if(X(i) == 1)
    {
      I(j++) = i;
    }
  }
}

template <typename DerivedP,
          typename DerivedF>
IGL_INLINE bool igl::predicates::ear_clipping(
  const Eigen::MatrixBase<DerivedP>& P,
  Eigen::PlainObjectBase<DerivedF>& eF)
{
  if(turning_number(P) < 0)
  {
    // reverse input
    const auto rP = P.colwise().reverse().eval();
    const bool ret = igl::predicates::ear_clipping(rP, eF);
    // flip output
    eF = eF.rowwise().reverse();
    return ret;
  }
  const Eigen::Matrix<typename DerivedP::Scalar, Eigen::Dynamic, 1> RT = 
    Eigen::Matrix<typename DerivedP::Scalar, Eigen::Dynamic, 1>::Zero(P.rows());
  Eigen::Matrix<typename DerivedF::Scalar, Eigen::Dynamic, 1> I;
  igl::predicates::ear_clipping(P, RT, eF, I);
  return I.size() == 0;
}

#ifdef IGL_STATIC_LIBRARY
template bool igl::predicates::ear_clipping<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&);
template void igl::predicates::ear_clipping<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&);
#endif
