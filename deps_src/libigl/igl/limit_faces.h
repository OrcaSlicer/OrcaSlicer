// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_LIMIT_FACES_H
#define IGL_LIMIT_FACES_H
#include "igl_inline.h"
namespace igl
{
  // LIMIT_FACES limit given faces F to those which contain (only) indices found
  // in L.
  //
  // [LF] = limit_faces(F,L,exclusive);
  // [LF,in] = limit_faces(F,L,exclusive);
  //
  // Templates:
  //   MatF matrix type of faces, matrixXi
  //   VecL  matrix type of vertex indices, VectorXi
  // Inputs:
  //   F  #F by 3 list of face indices
  //   L  #L by 1 list of allowed indices
  //   exclusive  flag specifying whether a face is included only if all its
  //     indices are in L, default is false
  // Outputs:
  //   LF  #LF by 3 list of remaining faces after limiting
  //   in  #F list of whether given face was included
  //
  template <typename MatF, typename VecL>
  IGL_INLINE void limit_faces(
    const MatF & F, 
    const VecL & L, 
    const bool exclusive,
    MatF & LF);
}

#ifndef IGL_STATIC_LIBRARY
#  include "limit_faces.cpp"
#endif

#endif
