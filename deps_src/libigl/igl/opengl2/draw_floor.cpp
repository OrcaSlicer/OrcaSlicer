// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "draw_floor.h"
#include "gl.h"

IGL_INLINE void igl::opengl2::draw_floor(const float * colorA, const float * colorB,
  const int GridSizeX,
  const int GridSizeY)
{
  const float SizeX = 0.5f*100./(double)GridSizeX;
  const float SizeY = 0.5f*100./(double)GridSizeY;
  // old settings
  int old_lighting=0,old_color_material=0;
  glGetIntegerv(GL_LIGHTING,&old_lighting);
  glGetIntegerv(GL_COLOR_MATERIAL,&old_color_material);
  glDisable(GL_LIGHTING);
  glColorMaterial( GL_FRONT, GL_EMISSION);
  glEnable(GL_COLOR_MATERIAL);
  glColorMaterial( GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
  // Set material
  const float black[] = {0.,0.,0.,1.};
  glMaterialfv(GL_FRONT, GL_AMBIENT, black);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, black);
  glMaterialfv(GL_FRONT, GL_SPECULAR, black);
  glMaterialfv(GL_FRONT, GL_EMISSION, black);
  glMaterialf(GL_FRONT, GL_SHININESS,0);
  const bool use_lighting = false;
  if(use_lighting)
  {
    glEnable(GL_LIGHTING);
  }else
  {
    glDisable(GL_LIGHTING);
  }


  glBegin(GL_QUADS);
  glNormal3f(0,1,0);
  for (int x =-GridSizeX/2;x<GridSizeX/2;++x)
  {
    for (int y =-GridSizeY/2;y<GridSizeY/2;++y)
    {
      if ((x+y)&0x00000001) //modulo 2
      {
        glColor4fv(colorA);
      }else
      {
        glColor4fv(colorB);
      }
      glVertex3f(    x*SizeX,0,(y+1)*SizeY);
      glVertex3f((x+1)*SizeX,0,(y+1)*SizeY);
      glVertex3f((x+1)*SizeX,0,    y*SizeY);
      glVertex3f(    x*SizeX,0,    y*SizeY);
    }
  }
  glEnd();

  (old_lighting ? glEnable(GL_LIGHTING) : glDisable(GL_LIGHTING));
  (old_color_material? glEnable(GL_COLOR_MATERIAL) : glDisable(GL_COLOR_MATERIAL));
}

IGL_INLINE void igl::opengl2::draw_floor()
{
  const float grey[] = {0.80,0.80,0.80,1.};
  const float white[] = {0.95,0.95,0.95,1.};
  igl::opengl2::draw_floor(grey,white);
}

IGL_INLINE void igl::opengl2::draw_floor_outline(const float * colorA, const float * colorB,
  const int GridSizeX,
  const int GridSizeY)
{
  const float SizeX = 0.5f*100./(double)GridSizeX;
  const float SizeY = 0.5f*100./(double)GridSizeY;
  float old_line_width =0;
  // old settings
  int old_lighting=0,old_color_material=0;
  glGetIntegerv(GL_LIGHTING,&old_lighting);
  glGetIntegerv(GL_COLOR_MATERIAL,&old_color_material);
  glDisable(GL_LIGHTING);

  // Set material
  const float black[] = {0.,0.,0.,1.};
  glMaterialfv(GL_FRONT, GL_AMBIENT, black);
  glMaterialfv(GL_FRONT, GL_DIFFUSE, black);
  glMaterialfv(GL_FRONT, GL_SPECULAR, black);
  glMaterialfv(GL_FRONT, GL_EMISSION, black);
  glMaterialf(GL_FRONT, GL_SHININESS,0);
  const bool use_lighting = false;
  if(use_lighting)
  {
    glEnable(GL_LIGHTING);
  }else
  {
    glDisable(GL_LIGHTING);
  }

  glLineWidth(2.0f);
  glBegin(GL_LINES);
  for (int x =-GridSizeX/2;x<=GridSizeX/2;++x)
  {
    if(x!=(GridSizeX/2))
    {
      for(int s = -1;s<2;s+=2)
      {
        int y = s*(GridSizeY/2);
        int cy = y==(GridSizeY/2) ? y-1 : y;
        if ((x+cy)&0x00000001) //modulo 2
        {
          glColor4fv(colorA);
          //glColor3f(1,0,0);
        }else
        {
          glColor4fv(colorB);
          //glColor3f(0,0,1);
        }
        glVertex3f((x+1)*SizeX,0,y*SizeY);
        glVertex3f(    x*SizeX,0,y*SizeY);
      }
    }
    if(x==-(GridSizeX/2) || x==(GridSizeX/2))
    {
      int cx = x==(GridSizeX/2) ? x-1 : x;
      for (int y =-GridSizeY/2;y<GridSizeY/2;++y)
      {
        if ((cx+y)&0x00000001) //modulo 2
        {
          glColor4fv(colorA);
          //glColor3f(1,0,0);
        }else
        {
          glColor4fv(colorB);
          //glColor3f(0,0,1);
        }
        glVertex3f(x*SizeX,0,(y+1)*SizeY);
        glVertex3f(x*SizeX,0,    y*SizeY);
      }
    }
  }
  glEnd();

  glGetFloatv(GL_LINE_WIDTH,&old_line_width);
  glLineWidth(old_line_width);
  (old_lighting ? glEnable(GL_LIGHTING) : glDisable(GL_LIGHTING));
  (old_color_material? glEnable(GL_COLOR_MATERIAL) : glDisable(GL_COLOR_MATERIAL));
}

IGL_INLINE void igl::opengl2::draw_floor_outline()
{
  const float grey[] = {0.80,0.80,0.80,1.};
  const float white[] = {0.95,0.95,0.95,1.};
  igl::opengl2::draw_floor_outline(grey,white);
}
#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
