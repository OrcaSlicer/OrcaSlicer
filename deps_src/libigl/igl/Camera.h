// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_CAMERA_H
#define IGL_CAMERA_H

// you're idiot, M$!
#if defined(_WIN32)
#undef far
#undef near
#endif

#include <Eigen/Geometry>
#include <Eigen/Core>
#include <PI.h>

#define IGL_CAMERA_MIN_ANGLE 5.0
namespace igl
{

  // A simple camera class. The camera stores projection parameters (field of
  // view angle, aspect ratio, near and far clips) as well as a rigid
  // transformation *of the camera as if it were also a scene object*. Thus, the
  // **inverse** of this rigid transformation is the modelview transformation.
  class Camera
  {
    public:
      // On windows you might need: -fno-delayed-template-parsing
      //static constexpr double IGL_CAMERA_MIN_ANGLE = 5.;
      //  m_angle  Field of view angle in degrees {45}
      //  m_aspect  Aspect ratio {1}
      //  m_near  near clipping plane {1e-2}
      //  m_far  far clipping plane {100}
      //  m_at_dist  distance of looking at point {1}
      //  m_orthographic  whether to use othrographic projection {false}
      //  m_rotation_conj  Conjugate of rotation part of rigid transformation of
      //    camera {identity}. Note: we purposefully store the conjugate because
      //    this is what TW_TYPE_QUAT4D is expecting.
      //  m_translation  Translation part of rigid transformation of camera
      //    {(0,0,1)}
      double m_angle, m_aspect, m_near, m_far, m_at_dist;
      bool m_orthographic;
      Eigen::Quaterniond m_rotation_conj;
      Eigen::Vector3d m_translation;
    public:
      inline Camera();
      inline virtual ~Camera(){}
      // Return projection matrix that takes relative camera coordinates and
      // transforms it to viewport coordinates
      //
      // Note:
      //
      //     if(m_angle > 0)
      //     {
      //       gluPerspective(m_angle,m_aspect,m_near,m_at_dist+m_far);
      //     }else
      //     {
      //       gluOrtho(-0.5*aspect,0.5*aspect,-0.5,0.5,m_at_dist+m_near,m_far);
      //     }
      //
      // Is equivalent to
      //
      //     glMultMatrixd(projection().data());
      //
      inline Eigen::Matrix4d projection() const;
      // Return an Affine transformation (rigid actually) that 
      // takes relative coordinates and tramsforms them into world 3d
      // coordinates: moves the camera into the scene.
      inline Eigen::Affine3d affine() const;
      // Return an Affine transformation (rigid actually) that puts the takes a
      // world 3d coordinate and transforms it into the relative camera
      // coordinates: moves the scene in front of the camera.
      //
      // Note:
      //
      //     gluLookAt(
      //       eye()(0), eye()(1), eye()(2),
      //       at()(0), at()(1), at()(2),
      //       up()(0), up()(1), up()(2));
      //
      // Is equivalent to
      //
      //     glMultMatrixd(camera.inverse().matrix().data());
      //
      // See also: affine, eye, at, up
      inline Eigen::Affine3d inverse() const;
      // Returns world coordinates position of center or "eye" of camera.
      inline Eigen::Vector3d eye() const;
      // Returns world coordinate position of a point "eye" is looking at.
      inline Eigen::Vector3d at() const;
      // Returns world coordinate unit vector of "up" vector
      inline Eigen::Vector3d up() const;
      // Return top right corner of unit plane in relative coordinates, that is
      // (w/2,h/2,1)
      inline Eigen::Vector3d unit_plane() const;
      // Move dv in the relative coordinate frame of the camera (move the FPS)
      //
      // Inputs:
      //   dv  (x,y,z) displacement vector
      //
      inline void dolly(const Eigen::Vector3d & dv);
      // "Scale zoom": Move `eye`, but leave `at`
      //
      // Input:
      //   s  amount to scale distance to at
      inline void push_away(const double s);
      // Aka "Hitchcock", "Vertigo", "Spielberg" or "Trombone" zoom:
      // simultaneously dolly while changing angle so that `at` not only stays
      // put in relative coordinates but also projected coordinates. That is
      //
      // Inputs:
      //   da  change in angle in degrees
      inline void dolly_zoom(const double da);
      // Turn around eye so that rotation is now q
      //
      // Inputs:
      //   q  new rotation as quaternion
      inline void turn_eye(const Eigen::Quaterniond & q);
      // Orbit around at so that rotation is now q
      //
      // Inputs:
      //   q  new rotation as quaternion
      inline void orbit(const Eigen::Quaterniond & q);
      // Rotate and translate so that camera is situated at "eye" looking at "at"
      // with "up" pointing up.
      //
      // Inputs:
      //   eye  (x,y,z) coordinates of eye position
      //   at   (x,y,z) coordinates of at position
      //   up   (x,y,z) coordinates of up vector
      inline void look_at(
        const Eigen::Vector3d & eye,
        const Eigen::Vector3d & at,
        const Eigen::Vector3d & up);
    // Needed any time Eigen Structures are used as class members
    // http://eigen.tuxfamily.org/dox-devel/group__TopicStructHavingEigenMembers.html
    public:
        EIGEN_MAKE_ALIGNED_OPERATOR_NEW
  };
}

// Implementation
#include "PI.h"
#include "EPS.h"
#include <cmath>
#include <iostream>
#include <cassert>

inline igl::Camera::Camera():
  m_angle(45.0),m_aspect(1),m_near(1e-2),m_far(100),m_at_dist(1),
  m_orthographic(false),
  m_rotation_conj(1,0,0,0),
  m_translation(0,0,1)
{
}

inline Eigen::Matrix4d igl::Camera::projection() const
{
  Eigen::Matrix4d P;
  using namespace std;
  const double far = m_at_dist + m_far;
  const double near = m_near;
  // http://stackoverflow.com/a/3738696/148668
  if(m_orthographic)
  {
    const double f = 0.5;
    const double left = -f*m_aspect;
    const double right = f*m_aspect;
    const double bottom = -f;
    const double top = f;
    const double tx = (right+left)/(right-left);
    const double ty = (top+bottom)/(top-bottom);
    const double tz = (far+near)/(far-near);
    const double z_fix = 0.5 /m_at_dist / tan(m_angle*0.5 * (igl::PI/180.) );
    P<<
      z_fix*2./(right-left), 0, 0, -tx,
      0, z_fix*2./(top-bottom), 0, -ty,
      0, 0, -z_fix*2./(far-near),  -tz,
      0, 0, 0, 1;
  }else
  {
    const double yScale = tan(PI*0.5 - 0.5*m_angle*PI/180.);
    // http://stackoverflow.com/a/14975139/148668
    const double xScale = yScale/m_aspect;
    P<< 
      xScale, 0, 0, 0,
      0, yScale, 0, 0,
      0, 0, -(far+near)/(far-near), -1,
      0, 0, -2.*near*far/(far-near), 0;
    P = P.transpose().eval();
  }
  return P;
}

inline Eigen::Affine3d igl::Camera::affine() const
{
  using namespace Eigen;
  Affine3d t = Affine3d::Identity();
  t.rotate(m_rotation_conj.conjugate());
  t.translate(m_translation);
  return t;
}

inline Eigen::Affine3d igl::Camera::inverse() const
{
  using namespace Eigen;
  Affine3d t = Affine3d::Identity();
  t.translate(-m_translation);
  t.rotate(m_rotation_conj);
  return t;
}

inline Eigen::Vector3d igl::Camera::eye() const
{
  using namespace Eigen;
  return affine() * Vector3d(0,0,0);
}

inline Eigen::Vector3d igl::Camera::at() const
{
  using namespace Eigen;
  return affine() * (Vector3d(0,0,-1)*m_at_dist);
}

inline Eigen::Vector3d igl::Camera::up() const
{
  using namespace Eigen;
  Affine3d t = Affine3d::Identity();
  t.rotate(m_rotation_conj.conjugate());
  return t * Vector3d(0,1,0);
}

inline Eigen::Vector3d igl::Camera::unit_plane() const
{
  // Distance of center pixel to eye
  const double d = 1.0;
  const double a = m_aspect;
  const double theta = m_angle*PI/180.;
  const double w =
    2.*sqrt(-d*d/(a*a*pow(tan(0.5*theta),2.)-1.))*a*tan(0.5*theta);
  const double h = w/a;
  return Eigen::Vector3d(w*0.5,h*0.5,-d);
}

inline void igl::Camera::dolly(const Eigen::Vector3d & dv)
{
  m_translation += dv;
}

inline void igl::Camera::push_away(const double s)
{
  using namespace Eigen;
#ifndef NDEBUG
  Vector3d old_at = at();
#endif
  const double old_at_dist = m_at_dist;
  m_at_dist = old_at_dist * s;
  dolly(Vector3d(0,0,1)*(m_at_dist - old_at_dist));
  assert((old_at-at()).squaredNorm() < DOUBLE_EPS);
}

inline void igl::Camera::dolly_zoom(const double da)
{
  using namespace std;
  using namespace Eigen;
#ifndef NDEBUG
  Vector3d old_at = at();
#endif
  const double old_angle = m_angle;
  if(old_angle + da < IGL_CAMERA_MIN_ANGLE)
  {
    m_orthographic = true;
  }else if(old_angle + da > IGL_CAMERA_MIN_ANGLE)
  {
    m_orthographic = false;
  }
  if(!m_orthographic)
  {
    m_angle += da;
    m_angle = min(89.,max(IGL_CAMERA_MIN_ANGLE,m_angle));
    // change in distance
    const double s = 
      (2.*tan(old_angle/2./180.*igl::PI)) /
      (2.*tan(m_angle/2./180.*igl::PI)) ;
    const double old_at_dist = m_at_dist;
    m_at_dist = old_at_dist * s;
    dolly(Vector3d(0,0,1)*(m_at_dist - old_at_dist));
    assert((old_at-at()).squaredNorm() < DOUBLE_EPS);
  }
}

inline void igl::Camera::turn_eye(const Eigen::Quaterniond & q)
{
  using namespace Eigen;
  Vector3d old_eye = eye();
  // eye should be fixed
  //
  // eye_1 = R_1 * t_1 = eye_0
  // t_1 = R_1' * eye_0
  m_rotation_conj = q.conjugate();
  m_translation = m_rotation_conj * old_eye;
  assert((old_eye - eye()).squaredNorm() < DOUBLE_EPS);
}

inline void igl::Camera::orbit(const Eigen::Quaterniond & q)
{
  using namespace Eigen;
  Vector3d old_at = at();
  // at should be fixed
  //
  // at_1 = R_1 * t_1 - R_1 * z = at_0
  // t_1 = R_1' * (at_0 + R_1 * z)
  m_rotation_conj = q.conjugate();
  m_translation = 
    m_rotation_conj * 
      (old_at + 
         m_rotation_conj.conjugate() * Vector3d(0,0,1) * m_at_dist);
  assert((old_at - at()).squaredNorm() < DOUBLE_EPS);
}

inline void igl::Camera::look_at(
  const Eigen::Vector3d & eye,
  const Eigen::Vector3d & at,
  const Eigen::Vector3d & up)
{
  using namespace Eigen;
  using namespace std;
  // http://www.opengl.org/sdk/docs/man2/xhtml/gluLookAt.xml
  // Normalize vector from at to eye
  Vector3d F = eye-at;
  m_at_dist = F.norm();
  F.normalize();
  // Project up onto plane orthogonal to F and normalize
  assert(up.cross(F).norm() > DOUBLE_EPS && "(eye-at) x up ≈ 0");
  const Vector3d proj_up = (up-(up.dot(F))*F).normalized();
  Quaterniond a,b;
  a.setFromTwoVectors(Vector3d(0,0,-1),-F);
  b.setFromTwoVectors(a*Vector3d(0,1,0),proj_up);
  m_rotation_conj = (b*a).conjugate();
  m_translation = m_rotation_conj * eye;
  //cout<<"m_at_dist: "<<m_at_dist<<endl;
  //cout<<"proj_up: "<<proj_up.transpose()<<endl;
  //cout<<"F: "<<F.transpose()<<endl;
  //cout<<"eye(): "<<this->eye().transpose()<<endl;
  //cout<<"at(): "<<this->at().transpose()<<endl;
  //cout<<"eye()-at(): "<<(this->eye()-this->at()).normalized().transpose()<<endl;
  //cout<<"eye-this->eye(): "<<(eye-this->eye()).squaredNorm()<<endl;
  assert(           (eye-this->eye()).squaredNorm() < DOUBLE_EPS);
  //assert((F-(this->eye()-this->at()).normalized()).squaredNorm() < 
  //  DOUBLE_EPS);
  assert(           (at-this->at()).squaredNorm() < DOUBLE_EPS);
  //assert(        (proj_up-this->up()).squaredNorm() < DOUBLE_EPS);
}

#endif
