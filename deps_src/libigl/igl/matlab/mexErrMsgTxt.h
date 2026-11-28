// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MATLAB_MEXERRMSGTXT_H
#define IGL_MATLAB_MEXERRMSGTXT_H
#include "../igl_inline.h"
// Overload mexErrMsgTxt to check an assertion then print text only if
// assertion fails
#include "mex.h"
namespace igl
{
  namespace matlab
  {
    /// Wrapper for mexErrMsgTxt that only calls error if test fails
    /// @param[in] test boolean expression to test
    /// @param[in] message message to print if test fails
    IGL_INLINE void mexErrMsgTxt(bool test, const char * message);
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "mexErrMsgTxt.cpp"
#endif
#endif
