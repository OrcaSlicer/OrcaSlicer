// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_MOUSECONTROLLER_H
#define IGL_OPENGL2_MOUSECONTROLLER_H
// Needs to be included before others
#include <Eigen/StdVector>
#include "RotateWidget.h"
#include "TranslateWidget.h"
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <vector>

// Class for control a skeletal FK rig with the mouse.
namespace igl
{
  namespace opengl2
  {
    class MouseController
    {
      public:
        typedef Eigen::VectorXi VectorXb;
        // Propagate selection to descendants so that selected bones and their
        // subtrees are all selected.
        //
        // Input:
        //   S  #S list of whether selected
        //   P  #S list of bone parents
        // Output:
        //   T  #S list of whether selected
        static inline void propogate_to_descendants_if(
          const VectorXb & S,
          const Eigen::VectorXi & P,
          VectorXb & T);
        // Create a matrix of colors for the selection and their descendants.
        //
        // Inputs:
        //   selection  #S list of whether a bone is selected
        //   selected_color  color for selected bones
        //   unselected_color  color for unselected bones
        // Outputs:
        //   C  #P by 4 list of colors
        static inline void color_if(
          const VectorXb & S,
          const Eigen::Vector4f & selected_color,
          const Eigen::Vector4f & unselected_color,
          Eigen::MatrixXf & C);
        enum WidgetMode
        {
          WIDGET_MODE_ROTATE = 0,
          WIDGET_MODE_TRANSLATE = 1,
          NUM_WIDGET_MODES = 2,
        };
      private:
        // m_is_selecting  whether currently selecting 
        // m_selection  #m_rotations list of whether a bone is selected
        // m_down_x  x-coordinate of mouse location at down
        // m_down_y  y-coordinate 〃
        // m_drag_x  x-coordinate of mouse location at drag
        // m_drag_y  y-coordinate 〃
        // m_widget  rotation widget for selected bone
        // m_width  width of containing window
        // m_height  height 〃
        // m_rotations  list of rotations for each bone
        // m_rotations_at_selection  list of rotations for each bone at time of
        //   selection
        // m_translations   list of translations for each bone
        // m_fk_rotations_at_selection  list of rotations for each bone at time of
        //   selection
        // m_root_enabled  Whether root is enabled
        bool m_is_selecting;
        VectorXb m_selection;
        int m_down_x,m_down_y,m_drag_x,m_drag_y;
        int m_width,m_height;
        igl::opengl2::RotateWidget m_widget;
        igl::opengl2::TranslateWidget m_trans_widget;
        Eigen::Quaterniond m_widget_rot_at_selection;
        //Eigen::Vector3d m_trans_widget_trans_at_selection;
        typedef std::vector<
          Eigen::Quaterniond,
          Eigen::aligned_allocator<Eigen::Quaterniond> > RotationList;
        typedef std::vector< Eigen::Vector3d > TranslationList;
        RotationList 
          m_rotations,
          m_rotations_at_selection,
          m_fk_rotations_at_selection,
          m_parent_rotations_at_selection;
        TranslationList 
          m_translations, 
          m_translations_at_selection,
          m_fk_translations_at_selection;
        bool m_root_enabled;
        WidgetMode m_widget_mode;
      public:
        MouseController();
        // Returns const reference to m_selection
        inline const VectorXb & selection() const{return m_selection;};
        //                          〃 m_is_selecting
        inline const bool & is_selecting() const{return m_is_selecting;}
        inline bool is_widget_down() const{return m_widget.is_down();}
        inline bool is_trans_widget_down() const{return m_trans_widget.is_down();}
        //                          〃 m_rotations
        inline const RotationList & rotations() const{return m_rotations;}
        inline const TranslationList & translations() const{return m_translations;}
        // Returns non-const reference to m_root_enabled
        inline bool & root_enabled(){ return m_root_enabled;}
        inline void reshape(const int w, const int h);
        // Process down, drag, up mouse events
        //
        // Inputs:
        //   x  x-coordinate of mouse click with respect to container
        //   y  y-coordinate 〃 
        // Returns true if accepted (action taken).
        inline bool down(const int x, const int y);
        inline bool drag(const int x, const int y);
        inline bool up(const int x, const int y);
        // Draw selection box and widget
        inline void draw() const;
        // Set `m_selection` based on the last drag selection and initialize
        // widget.
        //
        // Inputs:
        //   C  #C by dim list of joint positions at rest
        //   BE  #BE by 2 list of bone indices at rest
        //   P  #P list of bone parents
        inline void set_selection_from_last_drag(
          const Eigen::MatrixXd & C,
          const Eigen::MatrixXi & BE,
          const Eigen::VectorXi & P,
          const Eigen::VectorXi & RP);
        // Set from explicit selection
        inline void set_selection(
          const Eigen::VectorXi & S,
          const Eigen::MatrixXd & C,
          const Eigen::MatrixXi & BE,
          const Eigen::VectorXi & P,
          const Eigen::VectorXi & RP);
        // Set size of skeleton
        //
        // Inputs:
        //  n  number of bones
        inline void set_size(const int n);
        // Resets m_rotation elements to identity
        inline void reset();
        inline void reset_selected();
        inline void reset_rotations();
        inline void reset_selected_rotations();
        inline void reset_translations();
        inline void reset_selected_translations();
        inline bool set_rotations(const RotationList & vQ);
        inline bool set_translations(const TranslationList & vT);
        // Sets all entries in m_selection to false
        inline void clear_selection();
        // Returns true iff some element in m_selection is true
        inline bool any_selection() const;
        inline void set_widget_mode(const WidgetMode & mode);
      public:
          EIGEN_MAKE_ALIGNED_OPERATOR_NEW
    };
  }
}

// Implementation
#include "../line_segment_in_rectangle.h"
#include "draw_rectangular_marquee.h"
#include "project.h"
#include "../forward_kinematics.h"
#include <iostream>
#include <algorithm>
#include <functional>

inline void igl::opengl2::MouseController::propogate_to_descendants_if(
  const VectorXb & S,
  const Eigen::VectorXi & P,
  VectorXb & T)
{
  using namespace std;
  const int n = S.rows();
  assert(P.rows() == n);
  // dynamic programming
  T = S;
  vector<bool> seen(n,false);
  // Recursively look up chain and see if ancestor is selected
  const function<bool(int)> look_up = [&](int e) -> bool
  {
    if(e==-1)
    {
      return false;
    }
    if(!seen[e])
    {
      seen[e] = true;
      T(e) |= look_up(P(e));
    }
    return T(e);
  };
  for(int e = 0;e<n;e++)
  {
    if(!seen[e])
    {
      T(e) = look_up(e);
    }
  }
}

inline void igl::opengl2::MouseController::color_if(
  const VectorXb & S,
  const Eigen::Vector4f & selected_color,
  const Eigen::Vector4f & unselected_color,
  Eigen::MatrixXf & C)
{
  C.resize(S.rows(),4);
  for(int e=0;e<S.rows();e++)
  {
    C.row(e) = S(e)?selected_color:unselected_color;
  }
}

inline igl::opengl2::MouseController::MouseController():
  m_is_selecting(false),
  m_selection(),
  m_down_x(-1),m_down_y(-1),m_drag_x(-1),m_drag_y(-1),
  m_width(-1),m_height(-1),
  m_widget(),
  m_widget_rot_at_selection(),
  //m_trans_widget_trans_at_selection(),
  m_trans_widget(),
  m_rotations(),
  m_translations(),
  m_rotations_at_selection(),
  m_root_enabled(true),
  m_widget_mode(WIDGET_MODE_ROTATE)
{
}

inline void igl::opengl2::MouseController::reshape(const int w, const int h)
{
  m_width = w;
  m_height = h;
}

inline bool igl::opengl2::MouseController::down(const int x, const int y)
{
  using namespace std;
  m_down_x = m_drag_x =x;
  m_down_y = m_drag_y =y;
  const bool widget_down = any_selection() && 
    (
     (m_widget_mode == WIDGET_MODE_ROTATE && m_widget.down(x,m_height-y)) ||
     (m_widget_mode == WIDGET_MODE_TRANSLATE && 
        m_trans_widget.down(x,m_height-y))
    );
  if(!widget_down)
  {
    m_is_selecting = true;
  }
  return m_is_selecting || widget_down;
}

inline bool igl::opengl2::MouseController::drag(const int x, const int y)
{
  using namespace std;
  using namespace Eigen;
  m_drag_x = x;
  m_drag_y = y;
  if(m_is_selecting)
  {
    return m_is_selecting;
  }else
  {
    switch(m_widget_mode)
    {
      default: // fall through
      case WIDGET_MODE_ROTATE:
      {
        if(!m_widget.drag(x,m_height-y))
        {
          return false;
        }
        assert(any_selection());
        assert(m_selection.size() == (int)m_rotations.size());
        assert(m_selection.size() == (int)m_translations.size());
        for(int e = 0;e<m_selection.size();e++)
        {
          if(m_selection(e))
          {
            // Let:
            //     w.θr = w.θ ⋅ w.θ₀*  
            // w.θr takes (absolute) frame of w.θ₀ to w.θ:
            //     w.θ = w.θr ⋅ w.θ₀ 
            // Define:
            //     w.θ₀ = θfk ⋅ θx,
            // the absolute rotation of the x axis to the deformed bone at
            // selection. Likewise,
            //     w.θ = θfk' ⋅ θx,
            // the current absolute rotation of the x axis to the deformed bone.
            // Define recursively:
            //     θfk = θfk(p) ⋅ Θr,
            // then because we're only changeing this relative rotation
            //     θfk' = θfk(p) ⋅ Θr ⋅ θr* ⋅ θr'
            //     θfk' = θfk ⋅ θr* ⋅ θr'
            //     w.θ ⋅ θx* = θfk ⋅ θr* ⋅ θr'
            //     θr ⋅ θfk* ⋅ w.θ ⋅ θx* = θr'
            //     θr ⋅ θfk* ⋅ w.θr ⋅ w.θ₀ ⋅ θx* = θr'
            //     θr ⋅ θfk* ⋅ w.θr ⋅ θfk ⋅θx ⋅ θx* = θr'
            //     θr ⋅ θfk* ⋅ w.θr ⋅ θfk = θr'
            // which I guess is the right multiply change after being changed to
            // the bases of θfk, the rotation of the bone relative to its rest
            // frame.
            //
            const Quaterniond & frame = m_fk_rotations_at_selection[e];
            m_rotations[e] = 
              m_rotations_at_selection[e] *
              frame.conjugate() * 
              (m_widget.rot*m_widget_rot_at_selection.conjugate()) *
              frame;
          }
        }
      }
      case WIDGET_MODE_TRANSLATE:
      {
        if(!m_trans_widget.drag(x,m_height-y))
        {
          return false;
        }
        assert(any_selection());
        assert(m_selection.size() == (int)m_rotations.size());
        assert(m_selection.size() == (int)m_translations.size());
        for(int e = 0;e<m_selection.size();e++)
        {
          if(m_selection(e))
          {
            m_translations[e] = 
              m_translations_at_selection[e] + 
              m_parent_rotations_at_selection[e].conjugate()*
                m_trans_widget.m_trans;
          }
        }
      }
    }
    return true;
  }
}

inline bool igl::opengl2::MouseController::up(const int x, const int y)
{
  m_is_selecting = false;
  m_widget.up(x,m_height-y);
  m_trans_widget.up(x,m_height-y);
  return false;
}

inline void igl::opengl2::MouseController::draw() const
{
  if(any_selection())
  {
    switch(m_widget_mode)
    {
      default:
      case WIDGET_MODE_ROTATE:
        m_widget.draw();
        break;
      case WIDGET_MODE_TRANSLATE:
        m_trans_widget.draw();
        break;
    }
  }
  if(m_is_selecting)
  {
    // Remember settings
    GLboolean dt;
    glGetBooleanv(GL_DEPTH_TEST,&dt);
    int old_vp[4];
    glGetIntegerv(GL_VIEWPORT,old_vp);

    // True screen space
    glViewport(0,0,m_width,m_height);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0,m_width,0,m_height);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    draw_rectangular_marquee(
      m_down_x,
      m_height-m_down_y,
      m_drag_x,
      m_height-m_drag_y);

    // Restore settings
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glViewport(old_vp[0],old_vp[1],old_vp[2],old_vp[3]);
    dt?glEnable(GL_DEPTH_TEST):glDisable(GL_DEPTH_TEST);

  }
}

inline void igl::opengl2::MouseController::set_selection_from_last_drag(
  const Eigen::MatrixXd & C,
  const Eigen::MatrixXi & BE,
  const Eigen::VectorXi & P,
  const Eigen::VectorXi & RP)
{
  using namespace Eigen;
  using namespace std;
  m_rotations_at_selection = m_rotations;
  m_translations_at_selection = m_translations;
  assert(BE.rows() == P.rows());
  m_selection = VectorXb::Zero(BE.rows());
  // m_rotation[e]  is the relative rotation stored at bone e (as seen by the
  //   joint traveling with its parent)
  // vQ[e]  is the absolute rotation of a bone at rest to its current position:
  //   vQ[e] = vQ[p(e)] * m_rotation[e]
  vector<Quaterniond,aligned_allocator<Quaterniond> > vQ;
  vector<Vector3d> vT;
  forward_kinematics(C,BE,P,m_rotations,m_translations,vQ,vT);
  // Loop over deformed bones
  for(int e = 0;e<BE.rows();e++)
  {
    Affine3d a = Affine3d::Identity();
    a.translate(vT[e]);
    a.rotate(vQ[e]);
    Vector3d s = a * (Vector3d)C.row(BE(e,0));
    Vector3d d = a * (Vector3d)C.row(BE(e,1));
    Vector3d projs = project(s);
    Vector3d projd = project(d);
    m_selection(e) = line_segment_in_rectangle(
      projs.head(2),projd.head(2),
      Vector2d(m_down_x,m_height-m_down_y),
      Vector2d(m_drag_x,m_height-m_drag_y));
  }
  return set_selection(m_selection,C,BE,P,RP);
}

inline void igl::opengl2::MouseController::set_selection(
    const Eigen::VectorXi & S,
    const Eigen::MatrixXd & C,
    const Eigen::MatrixXi & BE,
    const Eigen::VectorXi & P,
    const Eigen::VectorXi & RP)
{
  using namespace Eigen;
  using namespace std;
  vector<Quaterniond,aligned_allocator<Quaterniond> > & vQ = 
    m_fk_rotations_at_selection;
  vector<Vector3d> & vT = m_fk_translations_at_selection;
  forward_kinematics(C,BE,P,m_rotations,m_translations,vQ,vT);
  m_parent_rotations_at_selection.resize(
    m_rotations.size(),Quaterniond::Identity());
  for(size_t r = 0;r<vQ.size();r++)
  {
    if(P(r)>=0)
    {
      m_parent_rotations_at_selection[r] = vQ[P(r)];
    }
  }


  if(&m_selection != &S)
  {
    m_selection = S;
  }
  assert(m_selection.rows() == BE.rows());
  assert(BE.rows() == P.rows());
  assert(BE.rows() == RP.rows());
  // Zero-out S up a path of ones from e
  auto propagate = [&](const int e, const VectorXb & S, VectorXb & N)
  {
    if(S(e))
    {
      int f = e;
      while(true)
      {
        int p = P(f);
        if(p==-1||!S(p))
        {
          break;
        }
        N(f) = false;
        f = p;
      }
    }
  };
  VectorXb prev_selection = m_selection;
  // Combine upward, group rigid parts, repeat
  while(true)
  {
    // Spread selection across rigid pieces
    VectorXb SRP(VectorXb::Zero(RP.maxCoeff()+1));
    for(int e = 0;e<BE.rows();e++)
    {
      SRP(RP(e)) |= m_selection(e);
    }
    for(int e = 0;e<BE.rows();e++)
    {
      m_selection(e) = SRP(RP(e));
    }
    // Clear selections below m_selection ancestors
    VectorXb new_selection = m_selection;
    for(int e = 0;e<P.rows();e++)
    {
      propagate(e,m_selection,new_selection);
    }
    m_selection = new_selection;
    if(m_selection==prev_selection)
    {
      break;
    }
    prev_selection = m_selection;
  }

  // Now selection should contain just bone roots of m_selection subtrees
  if(m_selection.array().any())
  {
    // Taking average 
    Vector3d avg_pos(0,0,0);
    //m_trans_widget_trans_at_selection.setConstant(0);
    m_widget_rot_at_selection.coeffs().setConstant(0);
    m_widget.rot.coeffs().array().setConstant(0);
    Quaterniond cur_rot(0,0,0,0);
    int num_selection = 0;
    // Compute average widget for selection
    for(int e = 0;e<BE.rows();e++)
    {
      if(m_selection(e))
      {
        Vector3d s = C.row(BE(e,0));
        Vector3d d = C.row(BE(e,1));
        auto b = (d-s).transpose().eval();
        {
          Affine3d a = Affine3d::Identity();
          a.translate(vT[e]);
          a.rotate(vQ[e]);
          avg_pos += a*s;
        }
        // Rotation of x axis to this bone
        Quaterniond rot_at_bind;
        rot_at_bind.setFromTwoVectors(Vector3d(1,0,0),b);
        const Quaterniond abs_rot = vQ[e] * rot_at_bind;
        m_widget_rot_at_selection.coeffs() += abs_rot.coeffs();
        //m_trans_widget_trans_at_selection += vT[e];
        num_selection++;
      }
    }
    // Take average
    avg_pos.array() /= (double)num_selection;
    //m_trans_widget_trans_at_selection.array() /= (double)num_selection;
    m_widget_rot_at_selection.coeffs().array() /= (double)num_selection;
    m_widget_rot_at_selection.normalize();
    m_widget.rot = m_widget_rot_at_selection;
    m_widget.pos      = avg_pos;
    m_trans_widget.m_pos = avg_pos;
    //m_trans_widget.m_trans = m_trans_widget_trans_at_selection;
    m_trans_widget.m_trans.setConstant(0);
  }
  m_widget.m_is_enabled = true;
  m_trans_widget.m_is_enabled = true;
  for(int s = 0;s<m_selection.rows();s++)
  {
    // a root is selected then disable.
    if(!m_root_enabled && m_selection(s) && P(s) == -1)
    {
      m_widget.m_is_enabled = false;
      m_trans_widget.m_is_enabled = false;
      break;
    }
  }
}

inline void igl::opengl2::MouseController::set_size(const int n)
{
  using namespace Eigen;
  clear_selection();
  m_rotations.clear();
  m_rotations.resize(n,Quaterniond::Identity());
  m_translations.clear();
  m_translations.resize(n,Vector3d(0,0,0));
  m_selection = VectorXb::Zero(n);
}

inline void igl::opengl2::MouseController::reset()
{
  reset_rotations();
  reset_translations();
}

inline void igl::opengl2::MouseController::reset_selected()
{
  reset_selected_rotations();
  reset_selected_translations();
}

inline void igl::opengl2::MouseController::reset_rotations()
{
  using namespace Eigen;
  using namespace std;
  fill(m_rotations.begin(),m_rotations.end(),Quaterniond::Identity());
  // cop out. just clear selection
  clear_selection();
}

inline void igl::opengl2::MouseController::reset_selected_rotations()
{
  using namespace Eigen;
  for(int e = 0;e<m_selection.size();e++)
  {
    if(m_selection(e))
    {
      m_rotations[e] = Quaterniond::Identity();
    }
  }
}

inline void igl::opengl2::MouseController::reset_translations()
{
  using namespace Eigen;
  using namespace std;
  fill(m_translations.begin(),m_translations.end(),Vector3d(0,0,0));
  // cop out. just clear selection
  clear_selection();
}

inline void igl::opengl2::MouseController::reset_selected_translations()
{
  using namespace Eigen;
  for(int e = 0;e<m_selection.size();e++)
  {
    if(m_selection(e))
    {
      m_translations[e] = Vector3d(0,0,0);
    }
  }
}

inline bool igl::opengl2::MouseController::set_rotations(const RotationList & vQ)
{
  if(vQ.size() != m_rotations.size())
  {
    return false;
  }
  assert(!any_selection());
  m_rotations = vQ;
  return true;
}

inline bool igl::opengl2::MouseController::set_translations(const TranslationList & vT)
{
  if(vT.size() != m_translations.size())
  {
    return false;
  }
  assert(!any_selection());
  m_translations = vT;
  return true;
}

inline void igl::opengl2::MouseController::clear_selection()
{
  m_selection.setConstant(false);
}

inline bool igl::opengl2::MouseController::any_selection() const
{
  return m_selection.array().any();
}

inline void igl::opengl2::MouseController::set_widget_mode(const WidgetMode & mode)
{
  switch(m_widget_mode)
  {
    default:
    case WIDGET_MODE_TRANSLATE:
      m_widget.pos = m_trans_widget.m_pos+m_trans_widget.m_trans;
      break;
    case WIDGET_MODE_ROTATE:
      break;
  }
  m_widget_mode = mode;
}

#endif
