// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "signed_distance_isosurface.h"
#include "point_mesh_squared_distance.h"
#include "complex_to_mesh.h"

#include "../../AABB.h"
#include "../../per_face_normals.h"
#include "../../per_edge_normals.h"
#include "../../per_vertex_normals.h"
#include "../../centroid.h"
#include "../../WindingNumberAABB.h"

#include <CGAL/Surface_mesh_default_triangulation_3.h>
#include <CGAL/Complex_2_in_triangulation_3.h>
#include <CGAL/make_surface_mesh.h>
#include <CGAL/Implicit_surface_3.h>
#include <CGAL/IO/output_surface_facets_to_polyhedron.h>
// Axis-aligned bounding box tree for tet tri intersection
#include <CGAL/AABB_tree.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_triangle_primitive.h>
#include <vector>

IGL_INLINE bool igl::copyleft::cgal::signed_distance_isosurface(
  const Eigen::MatrixXd & IV,
  const Eigen::MatrixXi & IF,
  const double level,
  const double angle_bound,
  const double radius_bound,
  const double distance_bound,
  const SignedDistanceType sign_type,
  Eigen::MatrixXd & V,
  Eigen::MatrixXi & F)
{
  using namespace std;
  using namespace Eigen;

  // default triangulation for Surface_mesher
  typedef CGAL::Surface_mesh_default_triangulation_3 Tr;
  // c2t3
  typedef CGAL::Complex_2_in_triangulation_3<Tr> C2t3;
  typedef Tr::Geom_traits GT;//Kernel
  typedef GT::Sphere_3 Sphere_3;
  typedef GT::Point_3 Point_3;
  typedef GT::FT FT;
  typedef std::function<FT (Point_3)> Function;
  typedef CGAL::Implicit_surface_3<GT, Function> Surface_3;

  AABB<Eigen::MatrixXd,3> tree;
  tree.init(IV,IF);

  Eigen::MatrixXd FN,VN,EN;
  Eigen::MatrixXi E;
  Eigen::VectorXi EMAP;
  WindingNumberAABB< Eigen::Vector3d, Eigen::MatrixXd, Eigen::MatrixXi > hier;
  switch(sign_type)
  {
    default:
      assert(false && "Unknown SignedDistanceType");
    case SIGNED_DISTANCE_TYPE_UNSIGNED:
      // do nothing
      break;
    case SIGNED_DISTANCE_TYPE_DEFAULT:
    case SIGNED_DISTANCE_TYPE_WINDING_NUMBER:
      hier.set_mesh(IV,IF);
      hier.grow();
      break;
    case SIGNED_DISTANCE_TYPE_PSEUDONORMAL:
      // "Signed Distance Computation Using the Angle Weighted Pseudonormal"
      // [Bærentzen & Aanæs 2005]
      per_face_normals(IV,IF,FN);
      per_vertex_normals(IV,IF,PER_VERTEX_NORMALS_WEIGHTING_TYPE_ANGLE,FN,VN);
      per_edge_normals(
        IV,IF,PER_EDGE_NORMALS_WEIGHTING_TYPE_UNIFORM,FN,EN,E,EMAP);
      break;
  }

  Tr tr;            // 3D-Delaunay triangulation
  C2t3 c2t3 (tr);   // 2D-complex in 3D-Delaunay triangulation
  // defining the surface
  const auto & IVmax = IV.colwise().maxCoeff();
  const auto & IVmin = IV.colwise().minCoeff();
  const double bbd = (IVmax-IVmin).norm();
  const double r = bbd/2.;
  const auto & IVmid = 0.5*(IVmax + IVmin);
  // Supposedly the implict needs to evaluate to <0 at cmid...
  // http://doc.cgal.org/latest/Surface_mesher/classCGAL_1_1Implicit__surface__3.html
  Point_3 cmid(IVmid(0),IVmid(1),IVmid(2));
  Function fun;
  switch(sign_type)
  {
    default:
      assert(false && "Unknown SignedDistanceType");
    case SIGNED_DISTANCE_TYPE_UNSIGNED:
      fun = 
        [&tree,&IV,&IF,&level](const Point_3 & q) -> FT
        {
          int i;
          RowVector3d c;
          const double sd = tree.squared_distance(
            IV,IF,RowVector3d(q.x(),q.y(),q.z()),i,c);
          return sd-level;
        };
    case SIGNED_DISTANCE_TYPE_DEFAULT:
    case SIGNED_DISTANCE_TYPE_WINDING_NUMBER:
      fun = 
        [&tree,&IV,&IF,&hier,&level](const Point_3 & q) -> FT
        {
          const double sd = signed_distance_winding_number(
            tree,IV,IF,hier,Vector3d(q.x(),q.y(),q.z()));
          return sd-level;
        };
      break;
    case SIGNED_DISTANCE_TYPE_PSEUDONORMAL:
      fun = [&tree,&IV,&IF,&FN,&VN,&EN,&EMAP,&level](const Point_3 & q) -> FT
        {
          const double sd = 
            igl::signed_distance_pseudonormal(
              tree,IV,IF,FN,VN,EN,EMAP,RowVector3d(q.x(),q.y(),q.z()));
          return sd- level;
        };
      break;
  }
  Sphere_3 bounding_sphere(cmid, (r+level)*(r+level));
  Surface_3 surface(fun,bounding_sphere);
  CGAL::Surface_mesh_default_criteria_3<Tr> 
    criteria(angle_bound,radius_bound,distance_bound);
  // meshing surface
  CGAL::make_surface_mesh(c2t3, surface, criteria, CGAL::Manifold_tag());
  // complex to (V,F)
  return igl::copyleft::cgal::complex_to_mesh(c2t3,V,F);
}
