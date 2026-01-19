// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_INIT_RENDER_TO_TEXTURE_H
#define IGL_OPENGL_INIT_RENDER_TO_TEXTURE_H
#include "../igl_inline.h"
#include "gl.h"
#include <cstdlib>
namespace igl
{
  namespace opengl
  {
    // Create a frame buffer that renders color to a RGBA texture a depth to a
    // "render buffer".
    //
    // After calling this, you can use with something like:
    //
    //     glBindFramebuffer(GL_FRAMEBUFFER, fbo_id);
    //     if(!depth_texture)
    //     {
    //       glBindRenderbuffer(GL_RENDERBUFFER, d_id);
    //     }
    //     //
    //     // draw scene ...
    //     //
    //     // clean up
    //     glBindFramebuffer(GL_FRAMEBUFFER,0);
    //     if(!depth_texture)
    //     {
    //       glBindRenderbuffer(GL_RENDERBUFFER, 0);
    //     }
    //     // Later ...
    //     glActiveTexture(GL_TEXTURE0+0);
    //     glBindTexture(GL_TEXTURE_2D,tex_id);
    //     if(depth_texture)
    //     {
    //       glActiveTexture(GL_TEXTURE0+1);
    //       glBindTexture(GL_TEXTURE_2D,d_id);
    //     }
    //     // draw textures
    //      
    //     
    //
    // Inputs:
    //   width  image width
    //   height  image height
    //   depth_texture  whether to create a texture for depth or to create a
    //     render buffer for depth 
    // Outputs:
    //   tex_id  id of the texture
    //   fbo_id  id of the frame buffer object
    //   d_id  id of the depth texture or frame buffer object
    //
    IGL_INLINE void init_render_to_texture(
      const size_t width,
      const size_t height,
      const bool depth_texture,
      GLuint & tex_id,
      GLuint & fbo_id,
      GLuint & d_id);
    // Wrapper with depth_texture = false for legacy reasons
    IGL_INLINE void init_render_to_texture(
      const size_t width,
      const size_t height,
      GLuint & tex_id,
      GLuint & fbo_id,
      GLuint & dfbo_id);
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "init_render_to_texture.cpp"
#endif
#endif
