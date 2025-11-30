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
  /// Boolean operation types
  enum MeshBooleanType
  {
    /// A ∪ B
    MESH_BOOLEAN_TYPE_UNION = 0,
    /// A ∩ B
    MESH_BOOLEAN_TYPE_INTERSECT = 1,
    /// A \ B
    MESH_BOOLEAN_TYPE_MINUS = 2,
    /// A ⊕ B
    MESH_BOOLEAN_TYPE_XOR = 3,
    /// Resolve intersections without removing any non-coplanar faces
    MESH_BOOLEAN_TYPE_RESOLVE = 4,
    /// Total number of Boolean options
    NUM_MESH_BOOLEAN_TYPES = 5
  };
};

#endif
