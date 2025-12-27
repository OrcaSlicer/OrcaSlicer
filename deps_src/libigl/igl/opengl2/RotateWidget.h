// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_ROTATE_WIDGET_H
#define IGL_OPENGL2_ROTATE_WIDGET_H
#include "../material_colors.h"
#include <Eigen/Geometry>
#include <Eigen/Core>
#include <vector>

namespace igl
{
  namespace opengl2
  {
    // 3D Rotate tool widget similar to Maya's. Works best if field of view angle
    // is less than ~25.
    class RotateWidget
    {
        // If a is true then use A else use desaturated A
        static inline void glColor4fv(const bool a, const Eigen::Vector4f & A);
      public:
        inline static Eigen::Quaterniond axis_q(const int a);
        inline static Eigen::Vector3d view_direction(const int x, const int y);
        inline static Eigen::Vector3d view_direction(const Eigen::Vector3d & pos);
        Eigen::Vector3d pos;
        Eigen::Quaterniond rot,down_rot;
        Eigen::Vector2d down_xy,drag_xy,down_dir;
        Eigen::Vector3d udown,udrag;
        double outer_radius_on_screen;
        double outer_over_inner;
        bool m_is_enabled;
        enum DownType
        {
          DOWN_TYPE_X = 0,
          DOWN_TYPE_Y = 1,
          DOWN_TYPE_Z = 2,
          DOWN_TYPE_OUTLINE = 3,
          DOWN_TYPE_TRACKBALL = 4,
          DOWN_TYPE_NONE = 5,
          NUM_DOWN_TYPES = 6
        } down_type, selected_type;
        inline RotateWidget();
        // Vector from origin to mouse click "Unprojected" onto plane with depth of
        // origin and scale to so that outer radius is 1
        // 
        // Inputs:
        //   x  mouse x position
        //   y  mouse y position
        // Returns vector
        inline Eigen::Vector3d unproject_onto(const int x, const int y) const;
        // Shoot ray from mouse click to sphere
        //
        // Inputs:
        //   x  mouse x position
        //   y  mouse y position
        // Outputs:
        //   hit  position of hit
        // Returns true only if there was a hit
        inline bool intersect(
          const int x, 
          const int y, 
          Eigen::Vector3d & hit) const;
        inline double unprojected_inner_radius() const;
        inline bool down(const int x, const int y);
        inline bool drag(const int x, const int y);
        inline bool up(const int x, const int y);
        inline bool is_down() const;
        inline void draw() const;
        inline void draw_guide() const;
      public:
          EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    };
  }
}

// Implementation
#include "../PI.h"
#include "../EPS.h"
#include "../ray_sphere_intersect.h"
#include "../mat_to_quat.h"
#include "../trackball.h"
#include "gl.h"
#include "project.h"
#include "unproject.h"
#include <iostream>
#include <cassert>

inline void igl::opengl2::RotateWidget::glColor4fv(
    const bool a,
    const Eigen::Vector4f & A)
{
  if(a)
  {
    ::glColor4fv(A.data());
  }else
  {
    Eigen::Vector4f B;
    const double f = 0.95; // desaturate by 95%
    const double L = 0.3*A(0) + 0.6*A(1) + 0.1*A(2);
    B.head(3) = A.head(3).array() + f*(L-A.head(3).array());
    B(3) = A(3);
    ::glColor4fv(B.data());
  }
}

inline Eigen::Quaterniond igl::opengl2::RotateWidget::axis_q(const int a)
{
  assert(a<3 && a>=0);
  const Eigen::Quaterniond axes[3] = {
    Eigen::Quaterniond(Eigen::AngleAxisd(igl::PI*0.5,Eigen::Vector3d(0,1,0))),
    Eigen::Quaterniond(Eigen::AngleAxisd(igl::PI*0.5,Eigen::Vector3d(1,0,0))),
    Eigen::Quaterniond::Identity()};
  return axes[a];
}

inline Eigen::Vector3d igl::opengl2::RotateWidget::view_direction(const int x, const int y)
{
  using namespace Eigen;
  const Vector3d win_s(x,y,0), win_d(x,y,1);
  const Vector3d s = unproject(win_s);
  const Vector3d d = unproject(win_d);
  return d-s;
}

inline Eigen::Vector3d igl::opengl2::RotateWidget::view_direction(const Eigen::Vector3d & pos)
{
  using namespace Eigen;
  const Vector3d ppos = project(pos);
  return view_direction(ppos(0),ppos(1));
}

inline igl::opengl2::RotateWidget::RotateWidget():
  pos(0,0,0),
  rot(Eigen::Quaterniond::Identity()),
  down_rot(rot),
  down_xy(-1,-1),drag_xy(-1,-1),
  outer_radius_on_screen(91.),
  outer_over_inner(1.13684210526),
  m_is_enabled(true),
  down_type(DOWN_TYPE_NONE), 
  selected_type(DOWN_TYPE_NONE)
{
}

inline Eigen::Vector3d igl::opengl2::RotateWidget::unproject_onto(
  const int x, 
  const int y) const
{
  using namespace Eigen;
  // KNOWN BUG: This projects to same depths as pos. I think what we actually
  // want is The intersection with the plane perpendicular to the view
  // direction at pos. If the field of view angle is small then this difference
  // is negligible.
  //const Vector3d ppos = project(pos);
  //const Vector3d uxy = unproject( Vector3d(x,y,ppos(2)));
  // http://en.wikipedia.org/wiki/Line-plane_intersection
  //
  // Hrrmmm. There's still something wrong here if the ball's in the corner of
  // the screen. Am I somehow not accounting for perspective correctly?
  //
  // Q: What about just projecting the circle's equation and solving for the
  // distance?
  const Vector3d l0 = unproject(Vector3d(x,y,0));
  const Vector3d l = unproject(Vector3d(x,y,1))-l0;
  const Vector3d n = view_direction(pos);
  const double t = (pos-l0).dot(n)/l.dot(n);
  const Vector3d uxy = l0+t*l;
  return (uxy-pos)/unprojected_inner_radius()*outer_over_inner*outer_over_inner;
}

inline bool igl::opengl2::RotateWidget::intersect(
  const int x, 
  const int y,
  Eigen::Vector3d & hit) const
{
  using namespace Eigen;
  Vector3d view = view_direction(x,y);
  const Vector3d ppos = project(pos);
  Vector3d uxy = unproject(Vector3d(x,y,ppos(2)));
  double t0,t1;
  if(!ray_sphere_intersect(uxy,view,pos,unprojected_inner_radius(),t0,t1))
  {
    return false;
  }
  hit = uxy+t0*view;
  return true;
}


inline double igl::opengl2::RotateWidget::unprojected_inner_radius() const
{
  using namespace Eigen;
  Vector3d off,ppos,ppos_off,pos_off;
  project(pos,ppos);
  ppos_off = ppos;
  ppos_off(0) += outer_radius_on_screen/outer_over_inner;
  unproject(ppos_off,pos_off);
  return (pos-pos_off).norm();
}
inline bool igl::opengl2::RotateWidget::down(const int x, const int y)
{
  using namespace Eigen;
  using namespace std;
  if(!m_is_enabled)
  {
    return false;
  }
  down_type = DOWN_TYPE_NONE;
  selected_type = DOWN_TYPE_NONE;
  down_xy = Vector2d(x,y);
  drag_xy = down_xy;
  down_rot = rot;
  Vector3d ppos = project(pos);
  const double r = (ppos.head(2) - down_xy).norm();
  const double thresh = 3;
  if(fabs(r - outer_radius_on_screen)<thresh)
  {
    udown = unproject_onto(x,y);
    udrag = udown;
    down_type = DOWN_TYPE_OUTLINE;
    selected_type = DOWN_TYPE_OUTLINE;
    // project mouse to same depth as pos
    return true;
  }else if(r < outer_radius_on_screen/outer_over_inner+thresh*0.5)
  {
    Vector3d hit;
    const bool is_hit = intersect(down_xy(0),down_xy(1),hit);
    if(!is_hit)
    {
      //cout<<"~~~!is_hit"<<endl;
    }
    auto on_meridian = [&](
      const Vector3d & hit, 
      const Quaterniond & rot, 
      const Quaterniond & m,
      Vector3d & pl_hit) -> bool
    {
      // project onto rotate plane
      pl_hit = hit-pos;
      pl_hit = (m.conjugate()*rot.conjugate()*pl_hit).eval();
      pl_hit(2) = 0;
      pl_hit = (rot*m*pl_hit).eval();
      pl_hit.normalize();
      pl_hit *= unprojected_inner_radius();
      pl_hit += pos;
      return (project(pl_hit).head(2)-project(hit).head(2)).norm()<2*thresh;
    };
    udown = (hit-pos).normalized()/outer_radius_on_screen;
    udrag = udown;
    for(int a = 0;a<3;a++)
    {
      Vector3d pl_hit;
      if(on_meridian(hit,rot,Quaterniond(axis_q(a)),pl_hit))
      {
        udown = (pl_hit-pos).normalized()/outer_radius_on_screen;
        udrag = udown;
        down_type = DownType(DOWN_TYPE_X+a);
        selected_type = down_type;
        {
          Vector3d dir3 = axis_q(a).conjugate()*down_rot.conjugate()*(hit-pos);
          dir3 = AngleAxisd(-PI*0.5,Vector3d(0,0,1))*dir3;
          dir3 = (rot*axis_q(a)*dir3).eval();
          down_dir = (project((hit+dir3).eval())-project(hit)).head(2);
          down_dir.normalize();
          //// flip y because y coordinate is going to be given backwards in
          //// drag()
          //down_dir(1) *= -1;
        }
        return true;
      }
    }
    //assert(is_hit);
    down_type = DOWN_TYPE_TRACKBALL;
    selected_type = DOWN_TYPE_TRACKBALL;
    return true;
  }else
  {
    return false;
  }
}

inline bool igl::opengl2::RotateWidget::drag(const int x, const int y)
{
  using namespace std;
  using namespace Eigen;
  if(!m_is_enabled)
  {
    return false;
  }
  drag_xy = Vector2d(x,y);
  switch(down_type)
  {
    case DOWN_TYPE_NONE:
      return false;
    default:
    {
      const Quaterniond & q = axis_q(down_type-DOWN_TYPE_X);
      const double dtheta = -(drag_xy - down_xy).dot(down_dir)/
        outer_radius_on_screen/outer_over_inner*PI/2.;
      Quaterniond dq(AngleAxisd(dtheta,down_rot*q*Vector3d(0,0,1)));
      rot = dq * down_rot;
      udrag = dq * udown;
      return true;
    }
    case DOWN_TYPE_OUTLINE:
      {
        Vector3d ppos = project(pos);
        // project mouse to same depth as pos
        udrag = unproject_onto(x,y);
        const Vector2d A = down_xy - ppos.head(2);
        const Vector2d B = drag_xy - ppos.head(2);
        const double dtheta = atan2(A(0)*B(1)-A(1)*B(0),A(0)*B(0)+A(1)*B(1));
        Vector3d n = view_direction(pos).normalized();
        Quaterniond dq(AngleAxisd(dtheta,-n));
        //Vector3d n = udrag.cross(udown).normalized();
        //Quaterniond dq(AngleAxisd(fabs(dtheta),-n));
        rot = dq * down_rot;
      }
      return true;
    case DOWN_TYPE_TRACKBALL:
      {
        Vector3d ppos = project(pos);
        const double r = (double)outer_radius_on_screen/outer_over_inner*2.0;
        //const int h = w;
        Vector4i vp;
        glGetIntegerv(GL_VIEWPORT,vp.data());
        const int h = vp(3);
        Quaterniond dq;
        trackball(
          r,r,
          1,
          Quaterniond::Identity(),
          double(    down_xy(0)-ppos(0)    )+r/2.,
          double((h-down_xy(1))-(h-ppos(1)))+r/2.,
          double(             x-ppos(0)    )+r/2.,
          double(         (h-y)-(h-ppos(1)))+r/2.,
          dq);
        // We've computed change in rotation according to this view:
        // R = mv * r, R' = rot * (mv * r)
        // But we only want new value for r:
        // R' = mv * r'
        // mv * r' = rot * (mv * r)
        // r' = mv* * rot * mv * r
        Matrix4d mv;
        glGetDoublev(GL_MODELVIEW_MATRIX,mv.data());
        Quaterniond scene_rot;
        // Convert modelview matrix to quaternion
        mat4_to_quat(mv.data(),scene_rot.coeffs().data());
        scene_rot.normalize();
        rot = scene_rot.conjugate() * dq * scene_rot * down_rot;
      }
      return true;
  }
}

inline bool igl::opengl2::RotateWidget::up(const int /*x*/, const int /*y*/)
{
  // even if disabled process up
  down_type = DOWN_TYPE_NONE;
  return false;
}

inline bool igl::opengl2::RotateWidget::is_down() const
{
  return down_type != DOWN_TYPE_NONE;
}

inline void igl::opengl2::RotateWidget::draw() const
{
  using namespace Eigen;
  using namespace std;
  glPushAttrib(GL_ENABLE_BIT | GL_LIGHTING_BIT | GL_DEPTH_BUFFER_BIT | GL_LINE_BIT);
  glDisable(GL_CLIP_PLANE0);

  glDisable(GL_LIGHTING);
  glDisable(GL_DEPTH_TEST);
  glLineWidth(2.0);

  double r = unprojected_inner_radius();
  Vector3d view = view_direction(pos).normalized();

  auto draw_circle = [&](const bool cull)
  {
    Vector3d view = view_direction(pos).normalized();
    glBegin(GL_LINES);
    const double th_step = (2.0*igl::PI/100.0);
    for(double th = 0;th<2.0*igl::PI+th_step;th+=th_step)
    {
      Vector3d a(cos(th),sin(th),0.0);
      Vector3d b(cos(th+th_step),sin(th+th_step),0.0);
      if(!cull || (0.5*(a+b)).dot(view)<FLOAT_EPS)
      {
        glVertex3dv(a.data());
        glVertex3dv(b.data());
      }
    }
    glEnd();
  };


  glPushMatrix();
  glTranslated(pos(0),pos(1),pos(2));

  glScaled(r,r,r);
  // Draw outlines
  {
    glPushMatrix();
    glColor4fv(m_is_enabled,MAYA_GREY);
    Quaterniond q;
    q.setFromTwoVectors(Vector3d(0,0,1),view);
    glMultMatrixd(Affine3d(q).matrix().data());
    draw_circle(false);
    glScaled(outer_over_inner,outer_over_inner,outer_over_inner);
    if(selected_type == DOWN_TYPE_OUTLINE)
    {
      glColor4fv(m_is_enabled,MAYA_YELLOW);
    }else
    {
      glColor4fv(m_is_enabled,MAYA_CYAN);
    }
    draw_circle(false);
    glPopMatrix();
  }
  // Draw quartiles
  {
    glPushMatrix();
    glMultMatrixd(Affine3d(rot).matrix().data());
    if(selected_type == DOWN_TYPE_Z)
    {
      glColor4fv(m_is_enabled,MAYA_YELLOW);
    }else
    {
      glColor4fv(m_is_enabled,MAYA_BLUE);
    }
    draw_circle(true);
    if(selected_type == DOWN_TYPE_Y)
    {
      glColor4fv(m_is_enabled,MAYA_YELLOW);
    }else
    {
      glColor4fv(m_is_enabled,MAYA_GREEN);
    }
    glRotated(90.0,1.0,0.0,0.0);
    draw_circle(true);
    if(selected_type == DOWN_TYPE_X)
    {
      glColor4fv(m_is_enabled,MAYA_YELLOW);
    }else
    {
      glColor4fv(m_is_enabled,MAYA_RED);
    }
    glRotated(90.0,0.0,1.0,0.0);
    draw_circle(true);
    glPopMatrix();
  }
  glColor4fv(m_is_enabled,MAYA_GREY);
  draw_guide();
  glPopMatrix();

  glPopAttrib();
};

inline void igl::opengl2::RotateWidget::draw_guide() const
{
  using namespace Eigen;
  using namespace std;
  glPushAttrib(
    GL_DEPTH_BUFFER_BIT | 
    GL_ENABLE_BIT | 
    GL_POLYGON_BIT | 
    GL_POINT_BIT | 
    GL_TRANSFORM_BIT |
    GL_STENCIL_BUFFER_BIT |
    GL_LIGHTING_BIT);

  // http://www.codeproject.com/Articles/23444/A-Simple-OpenGL-Stipple-Polygon-Example-EP_OpenGL_
  const GLubyte halftone[] = {
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55,
    0xAA, 0xAA, 0xAA, 0xAA, 0x55, 0x55, 0x55, 0x55};


  switch(down_type)
  {
    case DOWN_TYPE_NONE:
    case DOWN_TYPE_TRACKBALL:
      goto finish;
    case DOWN_TYPE_OUTLINE:
      glScaled(outer_over_inner,outer_over_inner,outer_over_inner);
      break;
    default:
      break;
  }
  {
    const Vector3d nudown(udown.normalized()), 
      nudrag(udrag.normalized());
    glPushMatrix();
    glDisable(GL_CULL_FACE);
    glDisable(GL_POINT_SMOOTH);
    glPointSize(5.);
    glBegin(GL_POINTS);
    glVertex3dv(nudown.data());
    glVertex3d(0,0,0);
    glVertex3dv(nudrag.data());
    glEnd();
    glBegin(GL_LINE_STRIP);
    glVertex3dv(nudown.data());
    glVertex3d(0,0,0);
    glVertex3dv(nudrag.data());
    glEnd();
    glEnable(GL_POLYGON_STIPPLE);
    glPolygonStipple(halftone);
    glBegin(GL_TRIANGLE_FAN);
    glVertex3d(0,0,0);
    Quaterniond dq = rot * down_rot.conjugate();
    //dq.setFromTwoVectors(nudown,nudrag);
    for(double t = 0;t<1;t+=0.1)
    {
      const Vector3d p = Quaterniond::Identity().slerp(t,dq) * nudown;
      glVertex3dv(p.data());
    }
    glVertex3dv(nudrag.data());
    glEnd();
    glPopMatrix();
  }
finish:
  glPopAttrib();
}

#endif
