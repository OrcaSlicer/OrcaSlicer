// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "swept_volume_signed_distance.h"
#include "LinSpaced.h"
#include "flood_fill.h"
#include "signed_distance.h"
#include "AABB.h"
#include "pseudonormal_test.h"
#include "per_face_normals.h"
#include "per_vertex_normals.h"
#include "per_edge_normals.h"
#include <Eigen/Geometry>
#include <cmath>
#include <algorithm>

IGL_INLINE void igl::swept_volume_signed_distance(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const std::function<Eigen::Affine3d(const double t)> & transform,
  const size_t & steps,
  const Eigen::MatrixXd & GV,
  const Eigen::RowVector3i & res,
  const double h,
  const double isolevel,
  const Eigen::VectorXd & S0,
  Eigen::VectorXd & S)
{
  using namespace std;
  using namespace igl;
  using namespace Eigen;
  S = S0;
  const VectorXd t = igl::LinSpaced<VectorXd >(steps,0,1);
  const bool finite_iso = isfinite(isolevel);
  const double extension = (finite_iso ? isolevel : 0) + sqrt(3.0)*h;
  Eigen::AlignedBox3d box(
    V.colwise().minCoeff().array()-extension,
    V.colwise().maxCoeff().array()+extension);
  // Precomputation
  Eigen::MatrixXd FN,VN,EN;
  Eigen::MatrixXi E;
  Eigen::VectorXi EMAP;
  per_face_normals(V,F,FN);
  per_vertex_normals(V,F,PER_VERTEX_NORMALS_WEIGHTING_TYPE_ANGLE,FN,VN);
  per_edge_normals(
    V,F,PER_EDGE_NORMALS_WEIGHTING_TYPE_UNIFORM,FN,EN,E,EMAP);
  AABB<MatrixXd,3> tree;
  tree.init(V,F);
  for(int ti = 0;ti<t.size();ti++)
  {
    const Affine3d At = transform(t(ti));
    for(int g = 0;g<GV.rows();g++)
    {
      // Don't bother finding out how deep inside points are.
      if(finite_iso && S(g)==S(g) && S(g)<isolevel-sqrt(3.0)*h)
      {
        continue;
      }
      const RowVector3d gv = 
        (GV.row(g) - At.translation().transpose())*At.linear();
      // If outside of extended box, then consider it "far away enough"
      if(finite_iso && !box.contains(gv.transpose()))
      {
        continue;
      }
      RowVector3d c,n;
      int i;
      double sqrd,s;
      //signed_distance_pseudonormal(tree,V,F,FN,VN,EN,EMAP,gv,s,sqrd,i,c,n);
      const double min_sqrd = 
        finite_iso ? 
        pow(sqrt(3.)*h+isolevel,2) : 
        numeric_limits<double>::infinity();
      sqrd = tree.squared_distance(V,F,gv,min_sqrd,i,c);
      if(sqrd<min_sqrd)
      {
        pseudonormal_test(V,F,FN,VN,EN,EMAP,gv,i,c,s,n);
        if(S(g) == S(g))
        {
          S(g) = std::min(S(g),s*sqrt(sqrd));
        }else
        {
          S(g) = s*sqrt(sqrd);
        }
      }
    }
  }

  if(finite_iso)
  {
    flood_fill(res,S);
  }else
  {
#ifndef NDEBUG
    // Check for nans
    std::for_each(S.data(),S.data()+S.size(),[](const double s){assert(s==s);});
#endif
  }
}

IGL_INLINE void igl::swept_volume_signed_distance(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const std::function<Eigen::Affine3d(const double t)> & transform,
  const size_t & steps,
  const Eigen::MatrixXd & GV,
  const Eigen::RowVector3i & res,
  const double h,
  const double isolevel,
  Eigen::VectorXd & S)
{
  using namespace std;
  using namespace igl;
  using namespace Eigen;
  S = VectorXd::Constant(GV.rows(),1,numeric_limits<double>::quiet_NaN());
  return 
    swept_volume_signed_distance(V,F,transform,steps,GV,res,h,isolevel,S,S);
}
