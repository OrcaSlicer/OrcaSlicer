// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#ifndef IGL_COPYLEFT_RESOLVE_DUPLICATED_FACES
#define IGL_COPYLEFT_RESOLVE_DUPLICATED_FACES

#include "igl_inline.h"
#include <Eigen/Core>

namespace igl {

  // Resolve duplicated faces according to the following rules per unique face:
  //
  // 1. If the number of positively oriented faces equals the number of
  //    negatively oriented faces, remove all duplicated faces at this triangle.
  // 2. If the number of positively oriented faces equals the number of
  //    negatively oriented faces plus 1, keeps one of the positively oriented
  //    face.
  // 3. If the number of positively oriented faces equals the number of
  //    negatively oriented faces minus 1, keeps one of the negatively oriented
  //    face.
  // 4. If the number of postively oriented faces differ with the number of
  //    negativley oriented faces by more than 1, the mesh is not orientable.
  //    An exception will be thrown.
  //
  // Inputs:
  //   F1  #F1 by 3 array of input faces.
  //
  // Outputs:
  //   F2  #F2 by 3 array of output faces without duplicated faces.
  //   J   #F2 list of indices into F1.
  template<
    typename DerivedF1,
    typename DerivedF2,
    typename DerivedJ >
  IGL_INLINE void resolve_duplicated_faces(
      const Eigen::PlainObjectBase<DerivedF1>& F1,
      Eigen::PlainObjectBase<DerivedF2>& F2,
      Eigen::PlainObjectBase<DerivedJ>& J);

}

#ifndef IGL_STATIC_LIBRARY
#  include "resolve_duplicated_faces.cpp"
#endif

#endif
