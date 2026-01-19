// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_NORMALTYPE_H
#define IGL_NORMALTYPE_H

namespace igl
{
  // PER_VERTEX_NORMALS  Normals computed per vertex based on incident faces
  // PER_FACE_NORMALS  Normals computed per face
  // PER_CORNER_NORMALS  Normals computed per corner (aka wedge) based on
  //   incident faces without sharp edge
  enum NormalType
  {
    PER_VERTEX_NORMALS,
    PER_FACE_NORMALS,
    PER_CORNER_NORMALS
  };
#  define NUM_NORMAL_TYPE 3
}

#endif

