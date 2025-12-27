// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_TRANSLATE_WIDGET_H
#define IGL_OPENGL2_TRANSLATE_WIDGET_H
#include "../material_colors.h"
#include <Eigen/Geometry>
#include <Eigen/Core>
#include <vector>

namespace igl
{
  namespace opengl2
  {
    class TranslateWidget
    {
public:
      // m_pos  position of center
      // m_trans  translation vector
      // m_down_xy  mouse position on down
      // m_drag_xy  mouse position on drag
      // m_is_enabled  whether enabled
      Eigen::Vector3d m_pos,m_trans,m_down_trans;
      Eigen::Vector2d m_down_xy, m_drag_xy;
      bool m_is_enabled;
      double m_len;
      enum DownType
      {
        DOWN_TYPE_X = 0,
        DOWN_TYPE_Y = 1,
        DOWN_TYPE_Z = 2,
        DOWN_TYPE_CENTER = 3,
        DOWN_TYPE_NONE = 4,
        NUM_DOWN_TYPES = 5
      } m_down_type, m_selected_type;
      inline TranslateWidget(const Eigen::Vector3d & pos = Eigen::Vector3d(0,0,0));
      inline bool down(const int x, const int y);
      inline bool drag(const int x, const int y);
      inline bool up(const int x, const int y);
      inline bool is_down() const;
      inline void draw() const;
public:
      EIGEN_MAKE_ALIGNED_OPERATOR_NEW;
    };
  }
}

// Implementation
#include "project.h"
#include "unproject.h"

inline igl::opengl2::TranslateWidget::TranslateWidget(
  const Eigen::Vector3d & pos):
  m_pos(pos),
  m_trans(0,0,0),
  m_down_xy(-1,-1),
  m_drag_xy(-1,-1),
  m_is_enabled(true),
  m_len(50),
  m_down_type(DOWN_TYPE_NONE), 
  m_selected_type(DOWN_TYPE_NONE)
{
}

inline bool igl::opengl2::TranslateWidget::down(const int x, const int y)
{
  using namespace Eigen;
  using namespace std;
  if(!m_is_enabled)
  {
    return false;
  }
  m_down_trans = m_trans;
  m_down_xy = Vector2d(x,y);
  m_drag_xy = m_down_xy;
  m_down_type = DOWN_TYPE_NONE;
  m_selected_type = DOWN_TYPE_NONE;
  Vector3d ppos = project((m_pos+m_trans).eval());
  const double r = (ppos.head(2) - m_down_xy).norm();
  const double center_thresh = 10;
  if(r < center_thresh)
  {
    m_down_type = DOWN_TYPE_CENTER;
    m_selected_type = m_down_type;
    return true;
  }else if(r < m_len)
  {
    // Might be hit on lines
  }
  return false;
}

inline bool igl::opengl2::TranslateWidget::drag(const int x, const int y)
{
  using namespace std;
  using namespace Eigen;
  if(!m_is_enabled)
  {
    return false;
  }
  m_drag_xy = Vector2d(x,y);
  switch(m_down_type)
  {
    case DOWN_TYPE_NONE:
      return false;
    default:
    {
      Vector3d ppos = project((m_pos+m_trans).eval());
      Vector3d drag3(m_drag_xy(0),m_drag_xy(1),ppos(2));
      Vector3d down3(m_down_xy(0),m_down_xy(1),ppos(2));
      m_trans = m_down_trans + unproject(drag3)-unproject(down3);
      return true;
    }
  }
}

inline bool igl::opengl2::TranslateWidget::up(const int /*x*/, const int /*y*/)
{
  // even if disabled process up
  m_down_type = DOWN_TYPE_NONE;
  return false;
}

inline bool igl::opengl2::TranslateWidget::is_down() const
{
  return m_down_type != DOWN_TYPE_NONE;
}

inline void igl::opengl2::TranslateWidget::draw() const
{
  using namespace Eigen;
  using namespace std;
  glPushAttrib(GL_ENABLE_BIT | GL_LIGHTING_BIT | GL_DEPTH_BUFFER_BIT | GL_LINE_BIT);
  glDisable(GL_LIGHTING);
  glDisable(GL_DEPTH_TEST);
  glLineWidth(2.0);
  auto draw_axes = [&]()
  {
    glBegin(GL_LINES);
    glColor3f(1,0,0);
    glVertex3f(0,0,0);
    glVertex3f(1,0,0);
    glColor3f(0,1,0);
    glVertex3f(0,0,0);
    glVertex3f(0,1,0);
    glColor3f(0,0,1);
    glVertex3f(0,0,0);
    glVertex3f(0,0,1);
    glEnd();
  };
  auto draw_cube = []
  {
    glBegin(GL_LINES);
    glVertex3f(-1.0f, 1.0f, 1.0f);
    glVertex3f(1.0f, 1.0f, 1.0f);
    glVertex3f(1.0f, 1.0f, 1.0f);
    glVertex3f(1.0f, -1.0f, 1.0f);
    glVertex3f(1.0f, -1.0f, 1.0f);
    glVertex3f(-1.0f, -1.0f, 1.0f);
    glVertex3f(-1.0f, -1.0f, 1.0f);
    glVertex3f(-1.0f, 1.0f, 1.0f);
    glVertex3f(1.0f, 1.0f, 1.0f);
    glVertex3f(1.0f, 1.0f, -1.0f);
    glVertex3f(1.0f, 1.0f, -1.0f);
    glVertex3f(1.0f, -1.0f, -1.0f);
    glVertex3f(1.0f, -1.0f, -1.0f);	
    glVertex3f(1.0f, -1.0f, 1.0f);
    glVertex3f(1.0f, 1.0f, -1.0f);
    glVertex3f(-1.0f, 1.0f, -1.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glVertex3f(1.0f, -1.0f, -1.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glVertex3f(-1.0f, 1.0f, -1.0f);
    glVertex3f(-1.0f, 1.0f, -1.0f);
    glVertex3f(-1.0f, 1.0f, 1.0f);
    glVertex3f(-1.0f, -1.0f, 1.0f);
    glVertex3f(-1.0f, -1.0f, -1.0f);
    glEnd();
  };
  glPushMatrix();
  glTranslated( m_pos(0)+m_trans(0), m_pos(1)+m_trans(1), m_pos(2)+m_trans(2));

  {
    Vector3d off,ppos,ppos_off,pos_off;
    project((m_pos+m_trans).eval(),ppos);
    ppos_off = ppos;
    ppos_off(0) += m_len;
    unproject(ppos_off,pos_off);
    const double r = (m_pos+m_trans-pos_off).norm();
    glScaled(r,r,r);
  }

  draw_axes();
  glScaled(0.05,0.05,0.05);
  if(m_selected_type == DOWN_TYPE_CENTER)
  {
    glColor3fv(MAYA_YELLOW.data());
  }else
  {
    glColor3fv(MAYA_GREY.data());
  }
  draw_cube();
  glPopMatrix();
  glPopAttrib();
}

#endif
