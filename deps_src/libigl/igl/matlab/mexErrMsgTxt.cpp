// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "mexErrMsgTxt.h"

IGL_INLINE void igl::matlab::mexErrMsgTxt(bool assertion, const char * text)
{
  if(!assertion)
  {
    ::mexErrMsgTxt(text);
  }
}

