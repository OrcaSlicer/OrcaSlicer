// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2022 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "SelectionWidget.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <imgui_fonts_droid_sans.h>
#include <GLFW/glfw3.h>
#include "../../../PI.h"

namespace igl{ namespace opengl{ namespace glfw{ namespace imgui{

IGL_INLINE void SelectionWidget::init(Viewer *_viewer, ImGuiPlugin *_plugin)
{
  ImGuiWidget::init(_viewer,_plugin);
  std::cout<<R"(
igl::opengl::glfw::imgui::SelectionWidget usage:
  [drag]  Draw a 2D selection
  l       Turn on and toggle between lasso and polygonal lasso tool
  M,m     Turn on and toggle between rectangular and circular marquee tool
  V,v     Turn off interactive selection
)";
}
IGL_INLINE void SelectionWidget::draw()
{
  if(mode == OFF){ return; }
  // Is this call necessary?
  ImGui::GetIO();

  float width, height;
  float highdpi = 1.0;
  {
    int fwidth, fheight;
    glfwGetFramebufferSize(viewer->window, &fwidth, &fheight);
    int iwidth, iheight;
    glfwGetWindowSize(viewer->window, &iwidth, &iheight);
    highdpi = float(iwidth)/float(fwidth);
    // highdpi
    width = (float)iwidth;
    height = (float)iheight;
  }

  ImGui::SetNextWindowPos( ImVec2(0,0) );
  ImGui::SetNextWindowSize(ImVec2(width,height), ImGuiCond_Always);

  ImGui::Begin("testing", nullptr,
               ImGuiWindowFlags_NoBackground
               | ImGuiWindowFlags_NoTitleBar
               | ImGuiWindowFlags_NoResize
               | ImGuiWindowFlags_NoMove
               | ImGuiWindowFlags_NoScrollbar
               | ImGuiWindowFlags_NoSavedSettings
               | ImGuiWindowFlags_NoInputs);

  ImDrawList* list = ImGui::GetWindowDrawList();
  for(int pass = 0;pass<2;pass++)
  {
    for(auto & p : L)
    {
      list->PathLineTo({ highdpi*p(0),height-highdpi*p(1) });
    }
    const bool closed = !(mode==LASSO || mode==POLYGONAL_LASSO) || !(is_down || is_drawing);
    if(pass == 0)
    {
      list->PathStroke(IM_COL32(255, 255, 255, 255), closed, 2);
    }else
    {
      list->PathStroke(IM_COL32(0, 0, 0, 255), closed, 1);
    }
  }

  ImGui::End();

  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

}

IGL_INLINE bool SelectionWidget::mouse_down(int /*button*/, int modifier)
{
  if(mode == OFF || (modifier & IGL_MOD_ALT) ){ return false;}
  is_down = true;
  has_moved_since_down = false;
  if(!is_drawing)
  {
    L.clear();
    is_drawing = true;
  }
  M.row(0) = xy(viewer);
  M.row(1) = M.row(0);
  L.emplace_back(M.row(0));
  return true;
}

IGL_INLINE bool SelectionWidget::mouse_up(int /*button*/, int /*modifier*/)
{
  is_down = false;
  // are we done? Check first and last lasso point (need at least 3 (2 real
  // points + 1 mouse-mouse point))
  if(is_drawing &&
    (mode!=POLYGONAL_LASSO ||(L.size()>=3&&(L[0]-L[L.size()-1]).norm()<=10.0)))
  {
    if(callback){ callback();}
    is_drawing = false;
  }
  return false;
}

IGL_INLINE bool SelectionWidget::mouse_move(int /*mouse_x*/, int /*mouse_y*/)
{
  if(!is_drawing){ return false; }
  if(!has_moved_since_down)
  {
    if(mode == POLYGONAL_LASSO) { L.emplace_back(L[L.size()-1]); }
    has_moved_since_down = true;
  }
  M.row(1) = xy(viewer);
  switch(mode)
  {
    case RECTANGULAR_MARQUEE:
      rect(M,L);
      break;
    case ELLIPTICAL_MARQUEE:
      circle(M,L);
      break;
    case POLYGONAL_LASSO:
      // Over write last point
      L[L.size()-1] = xy(viewer);
      break;
    case LASSO:
      L.emplace_back(xy(viewer));
      break;
    default: assert(false);
  }
  return true;
}

IGL_INLINE bool SelectionWidget::key_pressed(unsigned int key, int /*modifiers*/)
{
  Mode old = mode;
  if(OFF_KEY.find(char(key)) != std::string::npos)
  {
    mode = OFF;
  }else if(LASSO_KEY.find(char(key)) != std::string::npos)
  {
    if(mode == LASSO)
    {
      mode = POLYGONAL_LASSO;
    }else/*if(mode == POLYGONAL_LASSO)*/
    {
      mode = LASSO;
    }
  }else if(MARQUEE_KEY.find(char(key)) != std::string::npos)
  {
    if(mode == RECTANGULAR_MARQUEE)
    {
      mode = ELLIPTICAL_MARQUEE;
    }else/*if(mode == ELLIPTICAL_MARQUEE)*/
    {
      mode = RECTANGULAR_MARQUEE;
    }
  }
  if(old != mode)
  {
    clear();
    if(callback_post_mode_change){ callback_post_mode_change(old); }
    return true;
  }
  return false;
}

IGL_INLINE void SelectionWidget::clear()
{
  M.setZero();
  L.clear();
  is_drawing = false;
  is_down = false;
};

IGL_INLINE void SelectionWidget::circle(const Eigen::Matrix<float,2,2> & M,  std::vector<Eigen::RowVector2f> & L)
{
  L.clear();
  L.reserve(64);
  const float r = (M.row(1)-M.row(0)).norm();
  for(float th = 0;th<2.*igl::PI;th+=0.1)
  {
    L.emplace_back(M(0,0)+r*cos(th),M(0,1)+r*sin(th));
  }
}

IGL_INLINE void SelectionWidget::rect(const Eigen::Matrix<float,2,2> & M,  std::vector<Eigen::RowVector2f> & L)
{
  L.resize(4);
  L[0] = Eigen::RowVector2f(M(0,0),M(0,1));
  L[1] = Eigen::RowVector2f(M(1,0),M(0,1));
  L[2] = Eigen::RowVector2f(M(1,0),M(1,1));
  L[3] = Eigen::RowVector2f(M(0,0),M(1,1));
}

IGL_INLINE Eigen::RowVector2f SelectionWidget::xy(const Viewer * vr)
{
  return Eigen::RowVector2f(
    vr->current_mouse_x,
    vr->core().viewport(3) - vr->current_mouse_y);
}



}}}}
