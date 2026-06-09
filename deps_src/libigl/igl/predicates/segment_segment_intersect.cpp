// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2019 Hanxiao Shen <hanxiao@cs.nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "segment_segment_intersect.h"

// https://www.geeksforgeeks.org/check-if-two-given-line-segments-intersect/
template <typename DerivedP>
IGL_INLINE bool igl::predicates::segment_segment_intersect(
  const Eigen::MatrixBase<DerivedP>& a,
  const Eigen::MatrixBase<DerivedP>& b,
  const Eigen::MatrixBase<DerivedP>& c,
  const Eigen::MatrixBase<DerivedP>& d
)
{
  auto t1 = igl::predicates::orient2d(a,b,c);
  auto t2 = igl::predicates::orient2d(b,c,d);
  auto t3 = igl::predicates::orient2d(a,b,d);
  auto t4 = igl::predicates::orient2d(a,c,d);

  // assume m,n,p are colinear, check whether p is in range [m,n]
  auto on_segment = [](
    const Eigen::MatrixBase<DerivedP>& m,
    const Eigen::MatrixBase<DerivedP>& n,
    const Eigen::MatrixBase<DerivedP>& p
  ){
      return ((p(0) >= std::min(m(0),n(0))) &&
              (p(0) <= std::max(m(0),n(0))) &&
              (p(1) >= std::min(m(1),n(1))) &&
              (p(1) <= std::max(m(1),n(1))));
  };
  
  // colinear case
  if((t1 == igl::predicates::Orientation::COLLINEAR && on_segment(a,b,c)) ||
     (t2 == igl::predicates::Orientation::COLLINEAR && on_segment(c,d,b)) ||
     (t3 == igl::predicates::Orientation::COLLINEAR && on_segment(a,b,d)) ||
     (t4 == igl::predicates::Orientation::COLLINEAR && on_segment(c,d,a))) 
     return true;
  
  // ordinary case
  return (t1 != t3 && t2 != t4);
}

#ifdef IGL_STATIC_LIBRARY
template bool igl::predicates::segment_segment_intersect<Eigen::Matrix<double, 1, 2, 1, 1, 2> >(Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 1, 2, 1, 1, 2> > const&);
#endif
