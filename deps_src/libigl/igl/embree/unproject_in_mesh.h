// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_EMBREE_UNPROJECT_IN_MESH
#define IGL_EMBREE_UNPROJECT_IN_MESH
#include <igl/igl_inline.h>
#include <Eigen/Core>

#include <vector>
#include "../Hit.h"

namespace igl
{
  namespace embree
  {
    // Forward define
    class EmbreeIntersector;
  
    // Unproject a screen location (using current opengl viewport, projection, and
    // model view) to a 3D position _inside_ a given mesh. If the ray through the
    // given screen location (x,y) _hits_ the mesh more than twice then the 3D
    // midpoint between the first two hits is return. If it hits once, then that
    // point is return. If it does not hit the mesh then obj is not set.
    //
    //
    // Inputs:
    //    pos        screen space coordinates
    //    model      model matrix
    //    proj       projection matrix
    //    viewport   vieweport vector
    //    ei         EmbreeIntersector containing (V,F)
    // Outputs:
    //    obj        3d unprojected mouse point in mesh
    //    hits       vector of embree hits
    // Returns number of hits
    //
    // Note: Previous prototype did not require model, proj, and viewport. This
    // has been removed. Instead replace with:
    //
    //     Eigen::Matrix4f model,proj;
    //     Eigen::Vector4f viewport;
    //     igl::opengl2::model_proj_viewport(model,proj,viewport);
    //     igl::embree::unproject_in_mesh(Vector2f(x,y),model,proj,viewport,ei,obj,hits);
    //
    template < typename Derivedobj>
    IGL_INLINE int unproject_in_mesh(
      const Eigen::Vector2f& pos,
      const Eigen::Matrix4f& model,
      const Eigen::Matrix4f& proj,
      const Eigen::Vector4f& viewport,
      const EmbreeIntersector & ei,
      Eigen::PlainObjectBase<Derivedobj> & obj,
      std::vector<igl::Hit > & hits);
    template < typename Derivedobj>
    IGL_INLINE int unproject_in_mesh(
      const Eigen::Vector2f& pos,
      const Eigen::Matrix4f& model,
      const Eigen::Matrix4f& proj,
      const Eigen::Vector4f& viewport,
      const EmbreeIntersector & ei,
      Eigen::PlainObjectBase<Derivedobj> & obj);

  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "unproject_in_mesh.cpp"
#endif
#endif
