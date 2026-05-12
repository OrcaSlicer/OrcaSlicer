// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.


#ifndef IGL_DATED_COPY_H
#define IGL_DATED_COPY_H
#include "igl_inline.h"
#include <string>
namespace igl
{
  /// Copy the given file to a new file with the same basename in `dir`
  /// directory with the current date and time as a suffix.
  ///
  /// @param[in] src_path  path to source file
  /// @param[in] dir  directory of destination file
  /// @return whether the copy was successful
  ///
  /// #### Example:
  /// \code{cpp}
  ///   dated_copy("/path/to/foo","/bar/");
  ///   // copies /path/to/foo to /bar/foo-2013-12-12T18-10-56
  /// \endcode
  ///
  /// \bug This function does not work under windows
  IGL_INLINE bool dated_copy(const std::string & src_path, const std::string & dir);
  /// \overload
  ///
  /// \brief Uses current working directory.
  IGL_INLINE bool dated_copy(const std::string & src_path);
}
#ifndef IGL_STATIC_LIBRARY
#  include "dated_copy.cpp"
#endif
#endif 

