// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
// Copyright (C) 2021 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READ_FILE_BINARY_H
#define IGL_READ_FILE_BINARY_H

#include "igl_inline.h"

#include <cstdint>
#include <streambuf>
#include <istream>
#include <string>
#include <vector>

namespace igl {
  /// Read contents of file into a buffer of uint8_t bytes.
  ///
  /// @param[in,out] fp  pointer to open File
  /// @param[out] fileBufferBytes  contents of file as vector of bytes
  ///
  /// #### Side effects:
  ///   closes fp
  /// \throws runtime_error on error
  IGL_INLINE void read_file_binary(
    FILE *fp,
    std::vector<std::uint8_t> &fileBufferBytes);
}

#ifndef IGL_STATIC_LIBRARY
#include "read_file_binary.cpp"
#endif

#endif

