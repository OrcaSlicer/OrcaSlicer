// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2022 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_GLFW_IMGUI_IMGUIWIDGET_H
#define IGL_OPENGL_GLFW_IMGUI_IMGUIWIDGET_H

#include "ImGuiPlugin.h"
#include "ImGuiWidget.h"
#include "../../../igl_inline.h"
#include <memory>

namespace igl
{
  namespace opengl
  {
    namespace glfw
    {
      class Viewer;
      namespace imgui
      {
        // Forward declaration of the parent plugin
        class ImGuiPlugin;
        /// Abstract class for imgui "widgets". A widget is something that uses
        /// imgui, but doesn't own the entire imgui IO stack: the single
        /// ImGuiPlugin owns that and widgets are registered with it.
        class ImGuiWidget 
        {
          public:
            IGL_INLINE ImGuiWidget(){ name = "dummy"; }
            virtual ~ImGuiWidget(){}
            IGL_INLINE virtual void init(Viewer *_viewer, ImGuiPlugin *_plugin)
              { viewer = _viewer; plugin = _plugin; }
            IGL_INLINE virtual void shutdown() {}
            IGL_INLINE virtual void draw() {}
            IGL_INLINE virtual bool mouse_down(int /*button*/, int /*modifier*/)
              { return false;}
            IGL_INLINE virtual bool mouse_up(int /*button*/, int /*modifier*/)
              { return false;}
            IGL_INLINE virtual bool mouse_move(int /*mouse_x*/, int /*mouse_y*/)
              { return false;}
            IGL_INLINE virtual bool key_pressed(unsigned int /*key*/, int /*modifiers*/)
              { return false;}
            IGL_INLINE virtual bool key_down(int /*key*/, int /*modifiers*/)
              { return false;}
            IGL_INLINE virtual bool key_up(int /*key*/, int /*modifiers*/)
              { return false;}
            std::string name;
          protected:
            // Pointer to ImGuiPlugin's parent viewer
            Viewer *viewer;
            // Pointer to parent ImGuiPlugin class
            ImGuiPlugin *plugin;
        };

      }
    }
  }
}

#endif

