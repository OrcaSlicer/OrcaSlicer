// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL_CREATE_SHADER_PROGRAM_H
#define IGL_OPENGL_CREATE_SHADER_PROGRAM_H
#include "../igl_inline.h"
#include "gl.h"
#include <string>
#include <map>

namespace igl
{
  namespace opengl
  {
    /// Create a shader program with a vertex and fragments shader loading from
    /// source strings and vertex attributes assigned from a map before linking the
    /// shaders to the program, making it ready to use with glUseProgram(id)
    ///
    /// @param[in] geom_source  string containing source code of geometry shader (can be
    ///     "" to mean use default pass-through)
    /// @param[in] vert_source  string containing source code of vertex shader
    /// @param[in] frag_source  string containing source code of fragment shader
    /// @param[in] attrib  map containing table of vertex attribute strings add their
    ///            correspondingly ids (generated previously using glBindAttribLocation)
    /// @param[out] id  index id of created shader, set to 0 on error
    /// @return true on success, false on error
    ///
    /// \note Caller is responsible for making sure that current value of id is not
    /// leaking a shader (since it will be overwritten)
    ///
    /// \see destroy_shader_program
    IGL_INLINE bool create_shader_program(
      const std::string &geom_source,
      const std::string &vert_source,
      const std::string &frag_source,
      const std::map<std::string,GLuint> &attrib,
      GLuint & id);
    /// \overload
    IGL_INLINE bool create_shader_program(
      const std::string &vert_source,
      const std::string &frag_source,
      const std::map<std::string,GLuint> &attrib,
      GLuint & id);
    /// \overload
    ///
    /// @return index id of created shader, set to 0 on error
    IGL_INLINE GLuint create_shader_program(
      const std::string & geom_source,
      const std::string & vert_source,
      const std::string & frag_source,
      const std::map<std::string,GLuint> &attrib);
    /// \overload
    ///
    /// @return index id of created shader, set to 0 on error
    IGL_INLINE GLuint create_shader_program(
      const std::string & vert_source,
      const std::string & frag_source,
      const std::map<std::string,GLuint> &attrib);
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "create_shader_program.cpp"
#endif

#endif
