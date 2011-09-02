// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "validate_arg.h"
#include "requires_arg.h"
#include "mexErrMsgTxt.h"
#include "../C_STR.h"

IGL_INLINE void igl::matlab::validate_arg_scalar(
    const int i, const int nrhs, const mxArray * prhs[], const char * name)
{
  requires_arg(i,nrhs,name);
  mexErrMsgTxt(mxGetN(prhs[i+1])==1 && mxGetM(prhs[i+1])==1,
      C_STR("Parameter '"<<name<<"' requires scalar argument"));
}

IGL_INLINE void igl::matlab::validate_arg_logical(
    const int i, const int nrhs, const mxArray * prhs[], const char * name)
{
  requires_arg(i,nrhs,name);
  mexErrMsgTxt(mxIsLogical(prhs[i+1]),
    C_STR("Parameter '"<<name<<"' requires Logical argument"));
}

IGL_INLINE void igl::matlab::validate_arg_char(
    const int i, const int nrhs, const mxArray * prhs[], const char * name)
{
  requires_arg(i,nrhs,name);
  mexErrMsgTxt(mxIsChar(prhs[i+1]),
    C_STR("Parameter '"<<name<<"' requires char argument"));
}

IGL_INLINE void igl::matlab::validate_arg_double(
    const int i, const int nrhs, const mxArray * prhs[], const char * name)
{
  requires_arg(i,nrhs,name);
  mexErrMsgTxt(mxIsDouble(prhs[i+1]),
    C_STR("Parameter '"<<name<<"' requires double argument"));
}
IGL_INLINE void igl::matlab::validate_arg_function_handle(
    const int i, const int nrhs, const mxArray * prhs[], const char * name)
{
  requires_arg(i,nrhs,name);
  mexErrMsgTxt(mxIsClass(prhs[i+1],"function_handle"),
    C_STR("Parameter '"<<name<<"' requires function handle argument"));
}
