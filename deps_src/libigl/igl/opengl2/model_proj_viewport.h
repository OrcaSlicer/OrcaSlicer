// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_OPENGL2_MODEL_PROJ_VIEW_H
#define IGL_OPENGL2_MODEL_PROJ_VIEW_H
#include "../igl_inline.h"
#include <Eigen/Core>
namespace igl
{
  namespace opengl2
  {
    // Collect the model-view, projection and viewport matrices
    //
    // Outputs:
    //   model  4x4 modelview matrix
    //   proj   4x4 projection matrix
    //   viewport  4x1 viewport vector
    //
    template <typename Derivedmodel, typename Derivedproj, typename Derivedviewport>
    IGL_INLINE void model_proj_viewport(
      Eigen::PlainObjectBase<Derivedmodel> & model, 
      Eigen::PlainObjectBase<Derivedproj> & proj, 
      Eigen::PlainObjectBase<Derivedviewport> & viewport);
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "model_proj_viewport.cpp"
#endif
#endif
