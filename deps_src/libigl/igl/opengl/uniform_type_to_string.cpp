// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "uniform_type_to_string.h"

IGL_INLINE std::string igl::opengl::uniform_type_to_string(const GLenum type)
{
  switch(type)
  {
    case GL_FLOAT:
      return "GL_FLOAT";
    case GL_FLOAT_VEC2:
      return "GL_FLOAT_VEC2";
    case GL_FLOAT_VEC3:
      return "GL_FLOAT_VEC3";
    case GL_FLOAT_VEC4:
      return "GL_FLOAT_VEC4";
    case GL_INT:
      return "GL_INT";
    case GL_INT_VEC2:
      return "GL_INT_VEC2";
    case GL_INT_VEC3:
      return "GL_INT_VEC3";
    case GL_INT_VEC4:
      return "GL_INT_VEC4";
    case GL_BOOL:
      return "GL_BOOL";
    case GL_BOOL_VEC2:
      return "GL_BOOL_VEC2";
    case GL_BOOL_VEC3:
      return "GL_BOOL_VEC3";
    case GL_BOOL_VEC4:
      return "GL_BOOL_VEC4";
    case GL_FLOAT_MAT2:
      return "GL_FLOAT_MAT2";
    case GL_FLOAT_MAT3:
      return "GL_FLOAT_MAT3";
    case GL_FLOAT_MAT4:
      return "GL_FLOAT_MAT4";
    case GL_FLOAT_MAT2x3:
      return "GL_FLOAT_MAT2x3";
    case GL_FLOAT_MAT2x4:
      return "GL_FLOAT_MAT2x4";
    case GL_FLOAT_MAT3x2:
      return "GL_FLOAT_MAT3x2";
    case GL_FLOAT_MAT3x4:
      return "GL_FLOAT_MAT3x4";
    case GL_FLOAT_MAT4x2:
      return "GL_FLOAT_MAT4x2";
    case GL_FLOAT_MAT4x3:
      return "GL_FLOAT_MAT4x3";
    case GL_SAMPLER_1D:
      return "GL_SAMPLER_1D";
    case GL_SAMPLER_2D:
      return "GL_SAMPLER_2D";
    case GL_SAMPLER_3D:
      return "GL_SAMPLER_3D";
    case GL_SAMPLER_CUBE:
      return "GL_SAMPLER_CUBE";
    case GL_SAMPLER_1D_SHADOW:
      return "GL_SAMPLER_1D_SHADOW";
    case GL_SAMPLER_2D_SHADOW:
      return "GL_SAMPLER_2D_SHADOW";
    default:
      return "UNKNOWN_TYPE";
  }
}
