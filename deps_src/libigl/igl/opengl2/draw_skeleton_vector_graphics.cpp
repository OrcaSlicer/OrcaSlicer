// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "draw_skeleton_vector_graphics.h"
#include "draw_point.h"
#include "gl.h"
#include "../material_colors.h"

IGL_INLINE void igl::opengl2::draw_skeleton_vector_graphics(
  const Eigen::MatrixXd & C,
  const Eigen::MatrixXi & BE)
{
  return draw_skeleton_vector_graphics(C,BE,BBW_POINT_COLOR,BBW_LINE_COLOR);
}

IGL_INLINE void igl::opengl2::draw_skeleton_vector_graphics(
  const Eigen::MatrixXd & C,
  const Eigen::MatrixXi & BE,
  const float * point_color,
  const float * line_color)
{
  using namespace Eigen;

  int old_lighting=0;
  double old_line_width=1;
  glGetIntegerv(GL_LIGHTING,&old_lighting);
  glGetDoublev(GL_LINE_WIDTH,&old_line_width);
  int cm;
  glGetIntegerv(GL_COLOR_MATERIAL,&cm);
  glDisable(GL_LIGHTING);
  glDisable(GL_LINE_STIPPLE);
  //glEnable(GL_POLYGON_OFFSET_FILL);
  glEnable(GL_COLOR_MATERIAL);
  glLineWidth(10.0);
  glColorMaterial(GL_FRONT_AND_BACK,GL_DIFFUSE);
  float mat_ambient[4] = {0.1,0.1,0.1,1.0};
  float mat_specular[4] = {0.0,0.0,0.0,1.0};
  float mat_shininess = 1;
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT,  mat_ambient);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT,  mat_ambient);
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mat_specular);
  glMaterialf( GL_FRONT_AND_BACK, GL_SHININESS, mat_shininess);

  for(int i = 0;i<3;i++)
  {
    switch(i)
    {
      case 0: glColor3fv(WHITE);      glLineWidth(10); break;
      case 1: glColor3fv(BLACK);      glLineWidth( 6); break;
      case 2: glColor3fv(line_color); glLineWidth( 4); break;
    }
    // Loop over bone edges
    glBegin(GL_LINES);
    for(int e = 0;e<BE.rows();e++)
    {
      RowVector3d tip = C.row(BE(e,0));
      RowVector3d tail = C.row(BE(e,1));
      glVertex3dv(tip.data());
      glVertex3dv(tail.data());
    }
    glEnd();
  }

  glColor3fv(point_color);
  for(int i = 0;i<C.rows();i++)
  {
    RowVector3d p = C.row(i);
    draw_point(p(0),p(1),p(2));
  }

  (cm ? glEnable(GL_COLOR_MATERIAL):glDisable(GL_COLOR_MATERIAL));
  //glDisable(GL_POLYGON_OFFSET_FILL);
  glEnable(GL_LIGHTING);
  (old_lighting ? glEnable(GL_LIGHTING) : glDisable(GL_LIGHTING));
  glLineWidth(old_line_width);
}

template <typename DerivedC, typename DerivedBE, typename DerivedT>
IGL_INLINE void igl::opengl2::draw_skeleton_vector_graphics(
  const Eigen::PlainObjectBase<DerivedC> & C,
  const Eigen::PlainObjectBase<DerivedBE> & BE,
  const Eigen::PlainObjectBase<DerivedT> & T)
{
  return draw_skeleton_vector_graphics(C,BE,T,BBW_POINT_COLOR,BBW_LINE_COLOR);
}

template <typename DerivedC, typename DerivedBE, typename DerivedT>
IGL_INLINE void igl::opengl2::draw_skeleton_vector_graphics(
  const Eigen::PlainObjectBase<DerivedC> & C,
  const Eigen::PlainObjectBase<DerivedBE> & BE,
  const Eigen::PlainObjectBase<DerivedT> & T,
  const float * point_color,
  const float * line_color)
{
  DerivedC CT;
  DerivedBE BET;
  const int dim = T.cols();
  assert(dim == C.cols());
  CT.resize(2*BE.rows(),C.cols());
  BET.resize(BE.rows(),2);
  for(int e = 0;e<BE.rows();e++)
  {
    BET(e,0) = 2*e;
    BET(e,1) = 2*e+1;
    const auto & c0 = C.row(BE(e,0));
    const auto & c1 = C.row(BE(e,1));
    const auto & L = T.block(e*(dim+1),0,dim,dim);
    const auto & t = T.block(e*(dim+1)+dim,0,1,dim);
    CT.row(2*e) =   c0 * L + t;
    CT.row(2*e+1) = c1 * L + t;
  }
  draw_skeleton_vector_graphics(CT,BET,point_color,line_color);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::opengl2::draw_skeleton_vector_graphics<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&);
#endif
