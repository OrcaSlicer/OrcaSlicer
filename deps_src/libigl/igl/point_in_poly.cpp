// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "point_in_poly.h"

bool IGL_INLINE igl::point_in_poly( const std::vector<std::vector<unsigned int > >&poly, 
            const unsigned int xt, 
            const unsigned int yt)
{
  int npoints= poly.size();
  unsigned int xnew,ynew;
  unsigned int xold,yold;
  unsigned int x1,y1;
  unsigned int x2,y2;
  int i;
  int inside=0;
  
  if (npoints < 3) {
    return(0);
  }
  xold=poly[npoints-1][0];
  yold=poly[npoints-1][1];
  for (i=0 ; i < npoints ; i++) {
    xnew=poly[i][0];
    ynew=poly[i][1];
    if (xnew > xold) {
      x1=xold;
      x2=xnew;
      y1=yold;
      y2=ynew;
    }
    else {
      x1=xnew;
      x2=xold;
      y1=ynew;
      y2=yold;
    }
    if ((xnew < xt) == (xt <= xold)          /* edge "open" at one end */
        && ((long)yt-(long)y1)*(long)(x2-x1)
        < ((long)y2-(long)y1)*(long)(xt-x1)) {
      inside=!inside;
    }
    xold=xnew;
    yold=ynew;
  }
  return (inside != 0);
}
