// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//               2014 Christian Schüller <schuellchr@gmail.com> 
//
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_HIT_H
#define IGL_HIT_H

namespace igl
{
  // Reimplementation of the embree::Hit struct from embree1.0
  // 
  // TODO: template on floating point type
  struct Hit
  {
    int id; // primitive id
    int gid; // geometry id
    float u,v; // barycentric coordinates
    float t; // distance = direction*t to intersection
  };
}
#endif 
