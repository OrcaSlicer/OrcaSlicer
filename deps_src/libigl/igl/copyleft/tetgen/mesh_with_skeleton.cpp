// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "mesh_with_skeleton.h"
#include "tetrahedralize.h"

#include "../../sample_edges.h"
#include "../../cat.h"

#include <iostream>
// Default settings pq2Y tell tetgen to mesh interior of triangle mesh and
// to produce a graded tet mesh
const static std::string DEFAULT_TETGEN_FLAGS = "pq2Y";

IGL_INLINE bool igl::copyleft::tetgen::mesh_with_skeleton(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXd & C,
  const Eigen::VectorXi & /*P*/,
  const Eigen::MatrixXi & BE,
  const Eigen::MatrixXi & CE,
  const int samples_per_bone,
  const std::string & tetgen_flags,
  Eigen::MatrixXd & VV,
  Eigen::MatrixXi & TT,
  Eigen::MatrixXi & FF)
{
  using namespace Eigen;
  using namespace std;
  const string eff_tetgen_flags = 
    (tetgen_flags.length() == 0?DEFAULT_TETGEN_FLAGS:tetgen_flags);
  // Collect all edges that need samples:
  MatrixXi BECE = cat(1,BE,CE);
  MatrixXd S;
  // Sample each edge with 10 samples. (Choice of 10 doesn't seem to matter so
  // much, but could under some circumstances)
  sample_edges(C,BECE,samples_per_bone,S);
  // Vertices we'll constrain tet mesh to meet
  MatrixXd VS = cat(1,V,S);
  // Use tetgen to mesh the interior of surface, this assumes surface:
  //   * has no holes
  //   * has no non-manifold edges or vertices
  //   * has consistent orientation
  //   * has no self-intersections
  //   * has no 0-volume pieces
  cerr<<"tetgen begin()"<<endl;
  int status = tetrahedralize( VS,F,eff_tetgen_flags,VV,TT,FF);
  cerr<<"tetgen end()"<<endl;
  if(FF.rows() != F.rows())
  {
    // Issue a warning if the surface has changed
    cerr<<"mesh_with_skeleton: Warning: boundary faces != input faces"<<endl;
  }
  if(status != 0)
  {
    cerr<<
      "***************************************************************"<<endl<<
      "***************************************************************"<<endl<<
      "***************************************************************"<<endl<<
      "***************************************************************"<<endl<<
      "* mesh_with_skeleton: tetgen failed. Just meshing convex hull *"<<endl<<
      "***************************************************************"<<endl<<
      "***************************************************************"<<endl<<
      "***************************************************************"<<endl<<
      "***************************************************************"<<endl;
    // If meshing convex hull then use more regular mesh
    status = tetrahedralize(VS,F,"q1.414",VV,TT,FF);
    // I suppose this will fail if the skeleton is outside the mesh
    assert(FF.maxCoeff() < VV.rows());
    if(status != 0)
    {
      cerr<<"mesh_with_skeleton: tetgen failed again."<<endl;
      return false;
    }
  }

  return true;
}

IGL_INLINE bool igl::copyleft::tetgen::mesh_with_skeleton(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXd & C,
  const Eigen::VectorXi & P,
  const Eigen::MatrixXi & BE,
  const Eigen::MatrixXi & CE,
  const int samples_per_bone,
  Eigen::MatrixXd & VV,
  Eigen::MatrixXi & TT,
  Eigen::MatrixXi & FF)
{
  return mesh_with_skeleton(
    V,F,C,P,BE,CE,samples_per_bone,DEFAULT_TETGEN_FLAGS,VV,TT,FF);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
