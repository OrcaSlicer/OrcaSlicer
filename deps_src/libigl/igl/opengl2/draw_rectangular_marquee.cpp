// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.


#include "draw_rectangular_marquee.h"
#include "gl.h"
#include "glu.h"
#include "../material_colors.h"

IGL_INLINE void igl::opengl2::draw_rectangular_marquee(
  const int from_x,
  const int from_y,
  const int to_x,
  const int to_y)
{
  using namespace std;
  int l;
  glGetIntegerv(GL_LIGHTING,&l);
  int s;
  glGetIntegerv(GL_LINE_STIPPLE,&s);
  double lw;
  glGetDoublev(GL_LINE_WIDTH,&lw);
  glDisable(GL_LIGHTING);
  // Screen space for this viewport
  GLint viewport[4];
  glGetIntegerv(GL_VIEWPORT,viewport);
  const int width = viewport[2];
  const int height = viewport[3];
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  gluOrtho2D(0,width,0,height);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  glEnable(GL_LINE_STIPPLE);
  glLineStipple(3,0xAAAA);
  glLineWidth(1);
  glColor4f(0.2,0.2,0.2,1);
  glBegin(GL_LINE_STRIP);
  glVertex2d(from_x,from_y);
  glVertex2d(to_x,from_y);
  glVertex2d(to_x,to_y);
  glVertex2d(from_x,to_y);
  glVertex2d(from_x,from_y);
  glEnd();

  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glLineWidth(lw);
  (s ? glEnable(GL_LINE_STIPPLE):glDisable(GL_LINE_STIPPLE));
  (l ? glEnable(GL_LIGHTING):glDisable(GL_LIGHTING));
}

