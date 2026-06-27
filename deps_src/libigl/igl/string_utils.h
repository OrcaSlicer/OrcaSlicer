// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_STRING_UTILS_H
#define IGL_STRING_UTILS_H

#include "igl_inline.h"

#include <string>

namespace igl {

  /// Check if a string starts with a given prefix.
  ///
  /// @param[in] str  string to check
  /// @param[in] prefix  prefix to check
  /// @return true if str starts with prefix, false otherwise
  ///
  /// \fileinfo
  IGL_INLINE bool starts_with(const std::string &str, const std::string &prefix);
  /// \overload
  ///
  /// \fileinfo
  IGL_INLINE bool starts_with(const char *str, const char* prefix);
}

#ifndef IGL_STATIC_LIBRARY
#  include "string_utils.cpp"
#endif

#endif
