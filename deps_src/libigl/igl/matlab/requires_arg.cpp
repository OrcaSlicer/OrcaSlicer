// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "requires_arg.h"
#include "mexErrMsgTxt.h"
#include "../C_STR.h"

IGL_INLINE void igl::matlab::requires_arg(const int i, const int nrhs, const char *name)
{
  mexErrMsgTxt((i+1)<nrhs,
      C_STR("Parameter '"<<name<<"' requires argument"));
}
