// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_COPYLEFT_CGAL_STRING_TO_MESH_BOOLEAN_H
#define IGL_COPYLEFT_CGAL_STRING_TO_MESH_BOOLEAN_H

#include "../../igl_inline.h"
#include "../../MeshBooleanType.h"
#include <string>

namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      /// Convert string to boolean type
      ///
      /// @param[in] s  string identifying type, one of the following:
      ///      "union","intersect","minus","xor","resolve"
      /// @param[out] type  type of boolean operation
      /// @return true only on success
      ///     
      IGL_INLINE bool string_to_mesh_boolean_type(
        const std::string & s,
        MeshBooleanType & type);
      /// \overload
      /// \brief Returns type without error handling
      IGL_INLINE MeshBooleanType string_to_mesh_boolean_type(
        const std::string & s);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "string_to_mesh_boolean_type.cpp"
#endif

#endif
