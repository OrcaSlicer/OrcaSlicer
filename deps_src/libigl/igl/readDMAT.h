// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_READDMAT_H
#define IGL_READDMAT_H
#include "igl_inline.h"
/// @file readDMAT.h
///
/// .dmat - dense matrices
/// ======================
/// 
/// ------------------------------------------------------------------------
/// 
/// A .dmat file contains a dense matrix in column major order. It can contain ASCII or binary data. Note that it is uncompressed so binary only reduces the file size by 50%. But writing and reading binary is usually faster. In MATLAB, binary is almost 100x faster.
/// 
/// ASCII
/// -----
/// 
/// The first line is a header containing:
/// 
///     [#cols] [#rows]
/// 
/// Then the coefficients are printed in column-major order separated by spaces.
/// 
/// Binary
/// ------
/// 
/// Binary files will also contain the ascii header, but it should read:
/// 
///     0 0
/// 
/// Then there should be another header containing the size of the binary part:
/// 
///     [#cols] [#rows]
/// 
/// Then coefficients are written in column-major order in Little-endian 8-byte double precision IEEE floating point format.
/// 
/// **Note:** Line endings must be `'\n'` aka `char(10)` aka line feeds.
///
/// #### Example:
///
///      The matrix m = [1 2 3; 4 5 6];
///
/// corresponds to a .dmat file containing:
///
///       3 2
///       1 4 2 5 3 6
#include <string>
#include <vector>
#include <Eigen/Core>
namespace igl
{
  /// Read a matrix from an .dmat file
  ///
  /// @param[in] file_name  path to .dmat file
  /// @param[out] W  eigen matrix containing read-in coefficients
  /// @return true on success, false on error
  ///
  template <typename DerivedW>
  IGL_INLINE bool readDMAT(const std::string file_name, 
    Eigen::PlainObjectBase<DerivedW> & W);
  /// \overload
  template <typename Scalar>
  IGL_INLINE bool readDMAT(
    const std::string file_name, 
    std::vector<std::vector<Scalar> > & W);
}

#ifndef IGL_STATIC_LIBRARY
#  include "readDMAT.cpp"
#endif

#endif
