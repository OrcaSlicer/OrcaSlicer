// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "unproject_in_mesh.h"
#include "EmbreeIntersector.h"
#include "../unproject_ray.h"
#include "../unproject_in_mesh.h"
#include <vector>

template <typename Derivedobj>
IGL_INLINE int igl::embree::unproject_in_mesh(
  const Eigen::Vector2f& pos,
  const Eigen::Matrix4f& model,
  const Eigen::Matrix4f& proj,
  const Eigen::Vector4f& viewport,
  const EmbreeIntersector & ei,
  Eigen::PlainObjectBase<Derivedobj> & obj,
  std::vector<igl::Hit > & hits)
{
  using namespace std;
  using namespace Eigen;
  const auto & shoot_ray = [&ei](
    const Eigen::Vector3f& s,
    const Eigen::Vector3f& dir,
    std::vector<igl::Hit> & hits)
  {
    int num_rays_shot;
    ei.intersectRay(s,dir,hits,num_rays_shot);
  };
  return igl::unproject_in_mesh(pos,model,proj,viewport,shoot_ray,obj,hits);
}

template <typename Derivedobj>
IGL_INLINE int igl::embree::unproject_in_mesh(
    const Eigen::Vector2f& pos,
    const Eigen::Matrix4f& model,
    const Eigen::Matrix4f& proj,
    const Eigen::Vector4f& viewport,
    const EmbreeIntersector & ei,
    Eigen::PlainObjectBase<Derivedobj> & obj)
{
  std::vector<igl::Hit> hits;
  return unproject_in_mesh(pos,model,proj,viewport,ei,obj,hits);
}


#ifdef IGL_STATIC_LIBRARY
template int igl::embree::unproject_in_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::Matrix<float, 2, 1, 0, 2, 1> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 1, 0, 4, 1> const&, igl::embree::EmbreeIntersector const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, std::vector<igl::Hit, std::allocator<igl::Hit> >&);
template int igl::embree::unproject_in_mesh<Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::Matrix<float, 2, 1, 0, 2, 1> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 1, 0, 4, 1> const&, igl::embree::EmbreeIntersector const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, std::vector<igl::Hit, std::allocator<igl::Hit> >&);
template int igl::embree::unproject_in_mesh<Eigen::Matrix<double, 3, 1, 0, 3, 1> >(Eigen::Matrix<float, 2, 1, 0, 2, 1> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 1, 0, 4, 1> const&, igl::embree::EmbreeIntersector const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> >&);
#endif
