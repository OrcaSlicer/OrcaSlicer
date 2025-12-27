// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MESH_BOOLEAN_TYPE_H
#define IGL_MESH_BOOLEAN_TYPE_H
namespace igl
{
  enum MeshBooleanType
  {
    MESH_BOOLEAN_TYPE_UNION = 0,
    MESH_BOOLEAN_TYPE_INTERSECT = 1,
    MESH_BOOLEAN_TYPE_MINUS = 2,
    MESH_BOOLEAN_TYPE_XOR = 3,
    MESH_BOOLEAN_TYPE_RESOLVE = 4,
    NUM_MESH_BOOLEAN_TYPES = 5
  };
};

#endif
