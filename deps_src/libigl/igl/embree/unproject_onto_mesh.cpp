// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "unproject_onto_mesh.h"
#include "EmbreeIntersector.h"
#include "../unproject_onto_mesh.h"
#include <vector>

IGL_INLINE bool igl::embree::unproject_onto_mesh(
  const Eigen::Vector2f& pos,
  const Eigen::MatrixXi& F,
  const Eigen::Matrix4f& model,
  const Eigen::Matrix4f& proj,
  const Eigen::Vector4f& viewport,
  const EmbreeIntersector & ei,
  int& fid,
  Eigen::Vector3f& bc)
{
  using namespace std;
  using namespace Eigen;
  const auto & shoot_ray = [&ei](
    const Eigen::Vector3f& s,
    const Eigen::Vector3f& dir,
    igl::Hit & hit)->bool
  {
    return ei.intersectRay(s,dir,hit);
  };
  return igl::unproject_onto_mesh(pos,model,proj,viewport,shoot_ray,fid,bc);
}

IGL_INLINE bool igl::embree::unproject_onto_mesh(
  const Eigen::Vector2f& pos,
  const Eigen::MatrixXi& F,
  const Eigen::Matrix4f& model,
  const Eigen::Matrix4f& proj,
  const Eigen::Vector4f& viewport,
  const EmbreeIntersector & ei,
  int& fid,
  int& vid)
{
  Eigen::Vector3f bc;
  if(!igl::embree::unproject_onto_mesh(pos,F,model,proj,viewport,ei,fid,bc))
  {
    return false;
  }
  int i;
  bc.maxCoeff(&i);
  vid = F(fid,i);
  return true;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
