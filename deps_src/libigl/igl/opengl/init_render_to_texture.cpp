// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "init_render_to_texture.h"
#include "gl.h"
#include <cassert>

IGL_INLINE void igl::opengl::init_render_to_texture(
  const size_t width,
  const size_t height,
  const bool depth_texture,
  GLuint & tex_id,
  GLuint & fbo_id,
  GLuint & d_id)
{
  using namespace std;
  // http://www.opengl.org/wiki/Framebuffer_Object_Examples#Quick_example.2C_render_to_texture_.282D.29
  const auto & gen_tex = [](GLuint & tex_id)
  {
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  };
  gen_tex(tex_id);
  //NULL means reserve texture memory, but texels are undefined
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_BGRA, GL_FLOAT, NULL);
  glBindTexture(GL_TEXTURE_2D, 0);

  glGenFramebuffers(1, &fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_id);
  //Attach 2D texture to this FBO
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_id, 0);
  if(depth_texture)
  {
    // Generate a depth texture
    gen_tex(d_id);
    glTexImage2D(
      GL_TEXTURE_2D,
      0,
      GL_DEPTH_COMPONENT32,
      width,
      height,
      0,
      GL_DEPTH_COMPONENT,
      GL_FLOAT,
      NULL);
    glFramebufferTexture2D(
      GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,d_id,0);
  }else
  {
    // Attach a depth buffer
    glGenRenderbuffers(1, &d_id);
    glBindRenderbuffer(GL_RENDERBUFFER, d_id);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,width,height);
    glFramebufferRenderbuffer(
      GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,d_id);
  }

  //Does the GPU support current FBO configuration?
  GLenum status;
  status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  assert(status == GL_FRAMEBUFFER_COMPLETE);
  // Unbind to clean up
  if(!depth_texture)
  {
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

IGL_INLINE void igl::opengl::init_render_to_texture(
  const size_t width,
  const size_t height,
  GLuint & tex_id,
  GLuint & fbo_id,
  GLuint & dfbo_id)
{
  return init_render_to_texture(width,height,false,tex_id,fbo_id,dfbo_id);
}
