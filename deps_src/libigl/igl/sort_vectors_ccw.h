// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#ifndef IGL_SORT_VECTORS_CCW
#define IGL_SORT_VECTORS_CCW
#include "igl_inline.h"

#include <Eigen/Core>

namespace igl {
  // Sorts a set of N coplanar vectors in a ccw order, and returns their order.
  // Optionally it also returns a copy of the ordered vector set, or the indices,
  // in the original unordered set, of the vectors in the ordered set (called here
  // the "inverse" set of indices).
  
  // Inputs:
  //   P               1 by 3N row vector of the vectors to be sorted, stacked horizontally
  //   N               #1 by 3 normal of the plane where the vectors lie
  // Output:
  //   order           N by 1 order of the vectors (indices of the unordered vectors into
  //                   the ordered vector set)
  //   sorted          1 by 3N row vector of the ordered vectors, stacked horizontally
  //   inv_order       N by 1 "inverse" order of the vectors (the indices of the ordered
  //                   vectors into the unordered vector set)
  //
  template <typename DerivedS, typename DerivedI>
  IGL_INLINE void sort_vectors_ccw(
                                   const Eigen::PlainObjectBase<DerivedS>& P,
                                   const Eigen::PlainObjectBase<DerivedS>& N,
                                   Eigen::PlainObjectBase<DerivedI> &order,
                                   Eigen::PlainObjectBase<DerivedS> &sorted,
                                   Eigen::PlainObjectBase<DerivedI> &inv_order);

   template <typename DerivedS, typename DerivedI>
   IGL_INLINE void sort_vectors_ccw(
                                    const Eigen::PlainObjectBase<DerivedS>& P,
                                    const Eigen::PlainObjectBase<DerivedS>& N,
                                    Eigen::PlainObjectBase<DerivedI> &order,
                                    Eigen::PlainObjectBase<DerivedS> &sorted);

    template <typename DerivedS, typename DerivedI>
    IGL_INLINE void sort_vectors_ccw(
                                     const Eigen::PlainObjectBase<DerivedS>& P,
                                     const Eigen::PlainObjectBase<DerivedS>& N,
                                     Eigen::PlainObjectBase<DerivedI> &order,
                                     Eigen::PlainObjectBase<DerivedI> &inv_order);


     template <typename DerivedS, typename DerivedI>
     IGL_INLINE void sort_vectors_ccw(
                                      const Eigen::PlainObjectBase<DerivedS>& P,
                                      const Eigen::PlainObjectBase<DerivedS>& N,
                                      Eigen::PlainObjectBase<DerivedI> &order);

};


#ifndef IGL_STATIC_LIBRARY
#include "sort_vectors_ccw.cpp"
#endif


#endif /* defined(IGL_FIELD_LOCAL_GLOBAL_CONVERSIONS) */
