// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "mosek_guarded.h"
#include <iostream>

IGL_INLINE MSKrescodee igl::mosek::mosek_guarded(const MSKrescodee r)
{
  using namespace std;
  if(r != MSK_RES_OK)
  {
    /* In case of an error print error code and description. */      
    char symname[MSK_MAX_STR_LEN];
    char desc[MSK_MAX_STR_LEN];
    MSK_getcodedesc(r,symname,desc);
    cerr<<"MOSEK ERROR ("<<r<<"): "<<symname<<" - '"<<desc<<"'"<<endl;
  }
  return r;
}

