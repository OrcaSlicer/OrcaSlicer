// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_DRAW_FLOOR_H
#define IGL_OPENGL2_DRAW_FLOOR_H
#include "../igl_inline.h"
namespace igl
{
  namespace opengl2
  {

    // Draw a checkerboard floor aligned with current (X,Z) plane using ../opengl/OpenGL_
    // calls. side=50 centered at (0,0):
    //   (-25,-25)-->(-25,25)-->(25,25)-->(25,-25)
    //
    // Use glPushMatrix(), glScaled(), glTranslated() to arrange the floor.
    // 
    // Inputs:
    //   colorA  float4 color
    //   colorB  float4 color
    //
    // Example:
    //   // Draw a nice floor
    //   glPushMatrix();
    //   glCullFace(GL_BACK);
    //   glEnable(GL_CULL_FACE);
    //   glEnable(GL_LIGHTING);
    //   glTranslated(0,-1,0);
    //   if(project(Vector3d(0,0,0))(2) - project(Vector3d(0,1,0))(2) > -FLOAT_EPS)
    //   {
    //     draw_floor_outline();
    //   }
    //   draw_floor();
    //   glPopMatrix();
    //   glDisable(GL_CULL_FACE);
    //
    IGL_INLINE void draw_floor(
      const float * colorA, 
      const float * colorB, 
      const int GridSizeX=100, 
      const int GridSizeY=100);
    // Wrapper with default colors
    IGL_INLINE void draw_floor();
    IGL_INLINE void draw_floor_outline(
      const float * colorA, 
      const float * colorB, 
      const int GridSizeX=100, 
      const int GridSizeY=100);
    // Wrapper with default colors
    IGL_INLINE void draw_floor_outline();
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "draw_floor.cpp"
#endif
#endif
