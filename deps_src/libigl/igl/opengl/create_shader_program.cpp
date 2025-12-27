// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "create_shader_program.h"

#include "load_shader.h"
#include "print_program_info_log.h"
#include <iostream>
#include <cstdio>

IGL_INLINE bool igl::opengl::create_shader_program(
  const std::string & geom_source,
  const std::string & vert_source,
  const std::string & frag_source,
  const std::map<std::string,GLuint> & attrib,
  GLuint & id)
{
  using namespace std;
  if(vert_source == "" && frag_source == "")
  {
    cerr<<
      "create_shader_program() could not create shader program,"
      " both .vert and .frag source given were empty"<<endl;
    return false;
  }

  // create program
  id = glCreateProgram();
  if(id == 0)
  {
    cerr<<"create_shader_program() could not create shader program."<<endl;
    return false;
  }
  GLuint g = 0,f = 0,v = 0;

  if(geom_source != "")
  {
    // load vertex shader
    g = igl::opengl::load_shader(geom_source.c_str(),GL_GEOMETRY_SHADER);
    if(g == 0)
    {
      cerr<<"geometry shader failed to compile."<<endl;
      return false;
    }
    glAttachShader(id,g);
  }

  if(vert_source != "")
  {
    // load vertex shader
    v = igl::opengl::load_shader(vert_source.c_str(),GL_VERTEX_SHADER);
    if(v == 0)
    {
      cerr<<"vertex shader failed to compile."<<endl;
      return false;
    }
    glAttachShader(id,v);
  }

  if(frag_source != "")
  {
    // load fragment shader
    f = igl::opengl::load_shader(frag_source.c_str(),GL_FRAGMENT_SHADER);
    if(f == 0)
    {
      cerr<<"fragment shader failed to compile."<<endl;
      return false;
    }
    glAttachShader(id,f);
  }

  // loop over attributes
  for(
    std::map<std::string,GLuint>::const_iterator ait = attrib.begin();
    ait != attrib.end();
    ait++)
  {
    glBindAttribLocation(
      id,
      (*ait).second,
      (*ait).first.c_str());
  }
  // Link program
  glLinkProgram(id);
  const auto & detach = [&id](const GLuint shader)
  {
    if(shader)
    {
      glDetachShader(id,shader);
      glDeleteShader(shader);
    }
  };
  detach(g);
  detach(f);
  detach(v);

  // print log if any
  igl::opengl::print_program_info_log(id);

  return true;
}

IGL_INLINE bool igl::opengl::create_shader_program(
  const std::string & vert_source,
  const std::string & frag_source,
  const std::map<std::string,GLuint> & attrib,
  GLuint & prog_id)
{
  return create_shader_program("",vert_source,frag_source,attrib,prog_id);
}


IGL_INLINE GLuint igl::opengl::create_shader_program(
  const std::string & geom_source,
  const std::string & vert_source,
  const std::string & frag_source,
  const std::map<std::string,GLuint> & attrib)
{
  GLuint prog_id = 0;
  create_shader_program(geom_source,vert_source,frag_source,attrib,prog_id);
  return prog_id;
}

IGL_INLINE GLuint igl::opengl::create_shader_program(
  const std::string & vert_source,
  const std::string & frag_source,
  const std::map<std::string,GLuint> & attrib)
{
  GLuint prog_id = 0;
  create_shader_program(vert_source,frag_source,attrib,prog_id);
  return prog_id;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif

