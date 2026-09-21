// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2022 Alec Jacobson <alecjacobson@gmail.com>
// Copyright (C) 2018 Jérémie Dumas <jeremie.dumas@ens-lyon.org>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_GLFW_IMGUI_IMGUIMENU_H
#define IGL_OPENGL_GLFW_IMGUI_IMGUIMENU_H

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
      namespace imgui
      {
        /// Widget for a menu bar and a viewer window.
        class ImGuiMenu : public ImGuiWidget
        {
          public:
            IGL_INLINE virtual void init(Viewer *_viewer, ImGuiPlugin *_plugin) override;
            IGL_INLINE virtual void shutdown() override;
            IGL_INLINE virtual void draw() override;
            // Can be overwritten by `callback_draw_viewer_window`
            IGL_INLINE virtual void draw_viewer_window();
            // Can be overwritten by `callback_draw_viewer_menu`
            IGL_INLINE virtual void draw_viewer_menu();
            // Can be overwritten by `callback_draw_custom_window`
            IGL_INLINE virtual void draw_custom_window() { }
            // Customizable callbacks
            std::function<void(void)> callback_draw_viewer_window;
            std::function<void(void)> callback_draw_viewer_menu;
            std::function<void(void)> callback_draw_custom_window;
            float menu_scaling()
              { return plugin->hidpi_scaling() / plugin->pixel_ratio(); }
        };

      }
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "ImGuiMenu.cpp"
#endif

#endif
