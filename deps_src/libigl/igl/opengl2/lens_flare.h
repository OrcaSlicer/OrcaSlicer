// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_LENS_FLARE_H
#define IGL_OPENGL2_LENS_FLARE_H

#include "../igl_inline.h"
#include "gl.h"
#include <Eigen/Core>
#include <vector>

namespace igl
{
  namespace opengl2
  {

    struct Flare{
      int type;             /* flare texture index, 0..5 */
      float scale;
      float loc;            /* position on axis */
      float color[3];
      Flare():
        type(-1),
        scale(0),
        loc(0)
      {}
      Flare(int type, float location, float scale, const float color[3], float colorScale) :
        type(type),
        scale(scale),
        loc(location)
      {
        this->color[0] = color[0] * colorScale;
        this->color[1] = color[1] * colorScale;
        this->color[2] = color[2] * colorScale;
      }
    };
    
    
    // Initialize shared data for lens flates
    //
    // Inputs:
    //   start_id   starting texture id location (should have at least id:id+16 free)
    // Outputs:
    //   shine  list of texture ids for shines
    //   flare  list of texture ids for flares
    IGL_INLINE void lens_flare_load_textures(
      std::vector<GLuint> & shine_ids,
      std::vector<GLuint> & flare_ids);
    
    // Create a set of lens flares
    //
    // Inputs:
    //   A  primary color
    //   B  secondary color
    //   C  secondary color
    // Outputs:
    //   flares  list of flare objects
    IGL_INLINE void lens_flare_create(
      const float * A,
      const float * B,
      const float * C,
      std::vector<Flare> & flares);
    
    // Draw lens flares
    //
    // Inputs:
    //   flares  list of Flare objects
    //   shine_ids  list of shine textures
    //   flare_ids  list of flare texture ids
    //   light  position of light
    //   near_clip  near clipping plane
    //   shine_tic  current "tic" in shine textures
    // Outputs:
    //   shine_tic  current "tic" in shine textures
    IGL_INLINE void lens_flare_draw(
      const std::vector<Flare> & flares,
      const std::vector<GLuint> & shine_ids,
      const std::vector<GLuint> & flare_ids,
      const Eigen::Vector3f & light,
      const float near_clip,
      int & shine_tic);
  }
};

#ifndef IGL_STATIC_LIBRARY
#  include "lens_flare.cpp"
#endif

#endif
