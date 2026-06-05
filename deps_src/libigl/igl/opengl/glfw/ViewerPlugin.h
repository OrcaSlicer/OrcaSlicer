// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_GLFW_VIEWERPLUGIN_H
#define IGL_OPENGL_GLFW_VIEWERPLUGIN_H

// TODO:
// * create plugins/skeleton.h
// * pass time in draw function
// * remove Preview3D from comments
// * clean comments
#include <string>
#include "../../igl_inline.h"
#include <vector>

namespace igl
{
  namespace opengl
  {
    namespace glfw
    {


      // Forward declaration of the viewer
      class Viewer;

      /// Abstract class for plugins
      /// All plugins MUST have this class as their parent and may implement any/all
      /// the callbacks marked `virtual` here.
      ///
      /// Return value of callbacks: returning true to any of the callbacks tells
      /// Viewer that the event has been handled and that it should not be passed to
      /// other plugins or to other internal functions of Viewer
      class ViewerPlugin
      {
        public:
          IGL_INLINE ViewerPlugin() {plugin_name = "dummy";}
          virtual ~ViewerPlugin(){}
          /// This function is called when the viewer is initialized (no mesh will be loaded at this stage)
          IGL_INLINE virtual void init(Viewer *_viewer) { viewer = _viewer; }
          /// This function is called before shutdown
          IGL_INLINE virtual void shutdown() { }
          /// This function is called before a mesh is loaded
          IGL_INLINE virtual bool load(std::string /*filename*/) { return false; }
          /// This function is called before a mesh is saved
          IGL_INLINE virtual bool save(std::string /*filename*/) { return false; }
          /// This function is called when the scene is serialized
          IGL_INLINE virtual bool serialize(std::vector<char>& /*buffer*/) const 
            { return false; }
          /// This function is called when the scene is deserialized
          IGL_INLINE virtual bool deserialize(const std::vector<char>& /*buffer*/)
            { return false; }
          /// Runs immediately after a new mesh has been loaded.
          IGL_INLINE virtual bool post_load() { return false; }
          /// This function is called before the draw procedure of Viewer
          IGL_INLINE virtual bool pre_draw() { return false; }
          /// This function is called after the draw procedure of Viewer
          IGL_INLINE virtual bool post_draw() { return false; }
          /// This function is called after the window has been resized
          IGL_INLINE virtual void post_resize(int /*w*/, int /*h*/) { }
          IGL_INLINE virtual bool mouse_down(int /*button*/, int /*modifier*/)
            { return false; }
          IGL_INLINE virtual bool mouse_up(int /*button*/, int /*modifier*/)
            { return false; }
          IGL_INLINE virtual bool mouse_move(int /*mouse_x*/, int /*mouse_y*/)
            { return false; }
          IGL_INLINE virtual bool mouse_scroll(float /*delta_y*/)
            { return false; }
          IGL_INLINE virtual bool key_pressed(unsigned int /*key*/, int /*modifiers*/)
            { return false; }
          IGL_INLINE virtual bool key_down(int /*key*/, int /*modifiers*/)
            { return false; }
          IGL_INLINE virtual bool key_up(int /*key*/, int /*modifiers*/)
            { return false; }
          std::string plugin_name;
        protected:
          // Pointer to the main Viewer class
          Viewer *viewer;
      };

      namespace serialization
      {
        inline void serialize(const ViewerPlugin& obj,std::vector<char>& buffer)
        {
          obj.serialize(buffer);
        }

        inline void deserialize(ViewerPlugin& obj,const std::vector<char>& buffer)
        {
          obj.deserialize(buffer);
        }
      }

    }
  }
}

#endif
