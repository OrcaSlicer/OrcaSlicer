// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_VALIDATE_ARG_H
#define IGL_VALIDATE_ARG_H
#include "../igl_inline.h"
#include <mex.h>
namespace igl
{
  namespace matlab
  {
    /// Throw an error if arg i+1 is not a scalar
    ///
    /// @param[in] i  index of current argument
    /// @param[in] nrhs  total number of arguments
    /// @param[in] prhs  pointer to arguments array
    /// @param[in] name   name of current argument
    IGL_INLINE void validate_arg_scalar(
      const int i, const int nrhs, const mxArray * prhs[], const char * name);
    /// \overload
    /// \breif Throw an error if arg i+1 is not logical
    IGL_INLINE void validate_arg_logical(
      const int i, const int nrhs, const mxArray * prhs[], const char * name);
    /// \overload
    /// \breif Throw an error if arg i+1 is not char
    IGL_INLINE void validate_arg_char(
      const int i, const int nrhs, const mxArray * prhs[], const char * name);
    /// \overload
    /// \breif Throw an error if arg i+1 is not double
    IGL_INLINE void validate_arg_double(
      const int i, const int nrhs, const mxArray * prhs[], const char * name);
    /// \overload
    /// \breif Throw an error if arg i+1 is not a function handle
    IGL_INLINE void validate_arg_function_handle(
      const int i, const int nrhs, const mxArray * prhs[], const char * name);
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "validate_arg.cpp"
#endif
#endif
