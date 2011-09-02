// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "print_gl_get.h"

#include <cstdio>
IGL_INLINE void igl::opengl2::print_gl_get(GLenum pname)
{
  double dM[16];

  int rows = 4;
  int cols = 4;
  switch(pname)
  {
    case GL_MODELVIEW_MATRIX:
    case GL_PROJECTION_MATRIX:
    {
      rows = 4;
      cols = 4;
      glGetDoublev(pname,dM);
      for(int i = 0;i<rows;i++)
      {
        for(int j = 0;j<cols;j++)
        {
          printf("%lg ",dM[j*rows+i]);
        }
        printf("\n");
      }
      break;
    }
    default:
      fprintf(stderr,"ERROR in print_gl_get(), gl enum not recognized.\n");
  }
}
