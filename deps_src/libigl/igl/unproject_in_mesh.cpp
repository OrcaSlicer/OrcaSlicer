// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "unproject_in_mesh.h"
#include "unproject_ray.h"
#include "ray_mesh_intersect.h"

template < typename Derivedobj>
  IGL_INLINE int igl::unproject_in_mesh(
      const Eigen::Vector2f& pos,
      const Eigen::Matrix4f& model,
      const Eigen::Matrix4f& proj,
      const Eigen::Vector4f& viewport,
      const std::function<
        void(
          const Eigen::Vector3f&,
          const Eigen::Vector3f&,
          std::vector<igl::Hit> &)
          > & shoot_ray,
      Eigen::PlainObjectBase<Derivedobj> & obj,
      std::vector<igl::Hit > & hits)
{
  using namespace std;
  using namespace Eigen;
  Vector3f s,dir;
  unproject_ray(pos,model,proj,viewport,s,dir);
  shoot_ray(s,dir,hits);
  switch(hits.size())
  {
    case 0:
      break;
    case 1:
    {
      obj = (s + dir*hits[0].t).cast<typename Derivedobj::Scalar>();
      break;
    }
    case 2:
    default:
    {
      obj = 0.5*((s + dir*hits[0].t) + (s + dir*hits[1].t)).cast<typename Derivedobj::Scalar>();
      break;
    }
  }
  return hits.size();
}

extern "C"
{
#include "raytri.c"
}

template < typename DerivedV, typename DerivedF, typename Derivedobj>
  IGL_INLINE int igl::unproject_in_mesh(
      const Eigen::Vector2f& pos,
      const Eigen::Matrix4f& model,
      const Eigen::Matrix4f& proj,
      const Eigen::Vector4f& viewport,
      const Eigen::PlainObjectBase<DerivedV> & V,
      const Eigen::PlainObjectBase<DerivedF> & F,
      Eigen::PlainObjectBase<Derivedobj> & obj,
      std::vector<igl::Hit > & hits)
{
  using namespace std;
  using namespace Eigen;
  const auto & shoot_ray = [&V,&F](
    const Eigen::Vector3f& s,
    const Eigen::Vector3f& dir,
    std::vector<igl::Hit> & hits)
  {
    ray_mesh_intersect(s,dir,V,F,hits);
  };
  return unproject_in_mesh(pos,model,proj,viewport,shoot_ray,obj,hits);
}

template < typename DerivedV, typename DerivedF, typename Derivedobj>
  IGL_INLINE int igl::unproject_in_mesh(
      const Eigen::Vector2f& pos,
      const Eigen::Matrix4f& model,
      const Eigen::Matrix4f& proj,
      const Eigen::Vector4f& viewport,
      const Eigen::PlainObjectBase<DerivedV> & V,
      const Eigen::PlainObjectBase<DerivedF> & F,
      Eigen::PlainObjectBase<Derivedobj> & obj)
{
  std::vector<igl::Hit> hits;
  return unproject_in_mesh(pos,model,proj,viewport,V,F,obj,hits);
}
#ifdef IGL_STATIC_LIBRARY
template int igl::unproject_in_mesh<Eigen::Matrix<double, 1, 3, 1, 1, 3> >(Eigen::Matrix<float, 2, 1, 0, 2, 1> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 1, 0, 4, 1> const&, std::function<void (Eigen::Matrix<float, 3, 1, 0, 3, 1> const&, Eigen::Matrix<float, 3, 1, 0, 3, 1> const&, std::vector<igl::Hit, std::allocator<igl::Hit> >&)> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 1, 3, 1, 1, 3> >&, std::vector<igl::Hit, std::allocator<igl::Hit> >&);
template int igl::unproject_in_mesh<Eigen::Matrix<double, 3, 1, 0, 3, 1> >(Eigen::Matrix<float, 2, 1, 0, 2, 1> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 1, 0, 4, 1> const&, std::function<void (Eigen::Matrix<float, 3, 1, 0, 3, 1> const&, Eigen::Matrix<float, 3, 1, 0, 3, 1> const&, std::vector<igl::Hit, std::allocator<igl::Hit> >&)> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> >&, std::vector<igl::Hit, std::allocator<igl::Hit> >&);
template int igl::unproject_in_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::Matrix<float, 2, 1, 0, 2, 1> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 1, 0, 4, 1> const&, std::function<void (Eigen::Matrix<float, 3, 1, 0, 3, 1> const&, Eigen::Matrix<float, 3, 1, 0, 3, 1> const&, std::vector<igl::Hit, std::allocator<igl::Hit> >&)> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, std::vector<igl::Hit, std::allocator<igl::Hit> >&);
template int igl::unproject_in_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 3, 1, 0, 3, 1> >(Eigen::Matrix<float, 2, 1, 0, 2, 1> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 4, 0, 4, 4> const&, Eigen::Matrix<float, 4, 1, 0, 4, 1> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 3, 1, 0, 3, 1> >&);
#endif
