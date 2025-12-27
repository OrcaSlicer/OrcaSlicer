// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "draw_skeleton_3d.h"
#include "../PI.h"
#include "../material_colors.h"
#include "gl.h"
#include <Eigen/Geometry>
#include <iostream>


template <
  typename DerivedC,
  typename DerivedBE,
  typename DerivedT,
  typename Derivedcolor>
IGL_INLINE void igl::opengl2::draw_skeleton_3d(
  const Eigen::PlainObjectBase<DerivedC> & C,
  const Eigen::PlainObjectBase<DerivedBE> & BE,
  const Eigen::PlainObjectBase<DerivedT> & T,
  const Eigen::PlainObjectBase<Derivedcolor> & color,
  const double half_bbd)
{
  // Note: Maya's skeleton *does* scale with the mesh suggesting a scale
  // parameter. Further, its joint balls are not rotated with the bones.
  using namespace Eigen;
  using namespace std;
  if(color.size() == 0)
  {
    return draw_skeleton_3d(C,BE,T,MAYA_SEA_GREEN,half_bbd);
  }
  assert(color.cols() == 4 || color.size() == 4);
  if(T.size() == 0)
  {
    typedef Eigen::Matrix<typename DerivedT::Scalar,Dynamic,Dynamic> Mat;
    Mat I = Mat::Identity(C.cols()+1,C.cols());
    Mat T = I.replicate(BE.rows(),1);
    // insane base case
    if(T.size() == 0)
    {
      return;
    }
    return draw_skeleton_3d(C,BE,T,color,half_bbd);
  }
  assert(T.rows() == BE.rows()*(C.cols()+1));
  assert(T.cols() == C.cols());
  // push old settings
  glPushAttrib(GL_LIGHTING_BIT);
  glPushAttrib(GL_LINE_BIT);
  glDisable(GL_LIGHTING);
  glLineWidth(1.0);

  auto draw_sphere = [](const double r)
  {
    auto draw_circle = []()
    {
      glBegin(GL_LINE_STRIP);
      for(double th = 0;th<2.0*igl::PI;th+=(2.0*igl::PI/30.0))
      {
        glVertex3d(cos(th),sin(th),0.0);
      }
      glVertex3d(cos(0),sin(0),0.0);
      glEnd();
    };
    glPushMatrix();
    glScaled(r,r,r);
    draw_circle();
    glRotated(90.0,1.0,0.0,0.0);
    draw_circle();
    glRotated(90.0,0.0,1.0,0.0);
    draw_circle();
    glPopMatrix();
  };
  auto draw_pyramid = []()
  {
    glBegin(GL_LINE_STRIP);
    glVertex3d(0, 1,-1);
    glVertex3d(0,-1,-1);
    glVertex3d(0,-1, 1);
    glVertex3d(0, 1, 1);
    glVertex3d(0, 1,-1);
    glEnd();
    glBegin(GL_LINES);
    glVertex3d(0, 1,-1);
    glVertex3d(1,0,0);
    glVertex3d(0,-1,-1);
    glVertex3d(1,0,0);
    glVertex3d(0,-1, 1);
    glVertex3d(1,0,0);
    glVertex3d(0, 1, 1);
    glVertex3d(1,0,0);
    glEnd();
  };

  // Loop over bones
  for(int e = 0;e < BE.rows();e++)
  {
    // Draw a sphere
    auto s = C.row(BE(e,0));
    auto d = C.row(BE(e,1));
    auto b = (d-s).transpose().eval();
    double r = 0.02*half_bbd;
    Matrix4d Te = Matrix4d::Identity();
    Te.block(0,0,3,4) = T.block(e*4,0,4,3).transpose();
    Quaterniond q;
    q.setFromTwoVectors(Vector3d(1,0,0),b);
    glPushMatrix();
    glMultMatrixd(Te.data());
    glTranslated(s(0),s(1),s(2));
    if(color.size() == 4)
    {
      glColor4d( color(0), color(1), color(2), color(3));
    }else
    {
      glColor4d( color(e,0), color(e,1), color(e,2), color(e,3));
    }
    draw_sphere(r);
    const double len = b.norm()-2.*r;
    if(len>=0)
    {
      auto u = b.normalized()*r;
      glPushMatrix();
      glTranslated(u(0),u(1),u(2));
      glMultMatrixd(Affine3d(q).matrix().data());
      glScaled(b.norm()-2.*r,r,r);
      draw_pyramid();
      glPopMatrix();
    }
    glTranslated(b(0),b(1),b(2));
    draw_sphere(r);
    glPopMatrix();
  }
  // Reset settings
  glPopAttrib();
  glPopAttrib();
}

template <typename DerivedC, typename DerivedBE, typename DerivedT>
IGL_INLINE void igl::opengl2::draw_skeleton_3d(
  const Eigen::PlainObjectBase<DerivedC> & C,
  const Eigen::PlainObjectBase<DerivedBE> & BE,
  const Eigen::PlainObjectBase<DerivedT> & T)
{
  return draw_skeleton_3d(C,BE,T,MAYA_SEA_GREEN);
}

template <typename DerivedC, typename DerivedBE>
IGL_INLINE void igl::opengl2::draw_skeleton_3d(
  const Eigen::PlainObjectBase<DerivedC> & C,
  const Eigen::PlainObjectBase<DerivedBE> & BE)
{
  return draw_skeleton_3d(C,BE,Eigen::MatrixXd(),MAYA_SEA_GREEN);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::opengl2::draw_skeleton_3d<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&);
template void igl::opengl2::draw_skeleton_3d<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&);
template void igl::opengl2::draw_skeleton_3d<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<float, 4, 1, 0, 4, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, 4, 1, 0, 4, 1> > const&, double);
template void igl::opengl2::draw_skeleton_3d<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<float, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> > const&, double);
#endif
