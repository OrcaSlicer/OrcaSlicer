// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "qslim.h"

#include "connect_boundary_to_infinity.h"
#include "decimate.h"
#include "edge_flaps.h"
#include "find.h"
#include "is_edge_manifold.h"
#include "max_faces_stopping_condition.h"
#include "per_vertex_point_to_plane_quadrics.h"
#include "qslim_optimal_collapse_edge_callbacks.h"
#include "placeholders.h"
#include "quadric_binary_plus_operator.h"
#include "remove_unreferenced.h"
#include "intersection_blocking_collapse_edge_callbacks.h"
#include "AABB.h"
#include "PlainMatrix.h"

IGL_INLINE bool igl::qslim(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const int max_m,
  const bool block_intersections,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I)
{
  using namespace igl;
  igl::AABB<Eigen::MatrixXd, 3> * tree = nullptr;
  if(block_intersections)
  {
    tree = new igl::AABB<Eigen::MatrixXd, 3>();
    tree->init(V,F);
  }

  // Original number of faces
  const int orig_m = F.rows();
  // Tracking number of faces
  int m = F.rows();
  typedef Eigen::MatrixXd DerivedV;
  typedef Eigen::MatrixXi DerivedF;
  PlainMatrix<DerivedV,Eigen::Dynamic> VO;
  PlainMatrix<DerivedF,Eigen::Dynamic> FO;
  igl::connect_boundary_to_infinity(V,F,VO,FO);
  // decimate will not work correctly on non-edge-manifold meshes. By extension
  // this includes meshes with non-manifold vertices on the boundary since these
  // will create a non-manifold edge when connected to infinity.
  if(!is_edge_manifold(FO))
  {
    return false;
  }
  // These will unfortunately be immediately recomputed in decimate.
  Eigen::VectorXi EMAP;
  Eigen::MatrixXi E,EF,EI;
  edge_flaps(FO,E,EMAP,EF,EI);
  // Quadrics per vertex
  typedef std::tuple<Eigen::MatrixXd,Eigen::RowVectorXd,double> Quadric;
  std::vector<Quadric> quadrics;
  per_vertex_point_to_plane_quadrics(VO,FO,EMAP,EF,EI,quadrics);
  // State variables keeping track of edge we just collapsed
  int v1 = -1;
  int v2 = -1;
  // Callbacks for computing and updating metric
  decimate_cost_and_placement_callback cost_and_placement;
  decimate_pre_collapse_callback       pre_collapse;
  decimate_post_collapse_callback      post_collapse;
  qslim_optimal_collapse_edge_callbacks(
    E,quadrics,v1,v2, cost_and_placement, pre_collapse,post_collapse);
  if(block_intersections)
  {
    igl::intersection_blocking_collapse_edge_callbacks(
      pre_collapse, post_collapse, // These will get copied as needed
      tree,
      pre_collapse, post_collapse);
  }
  // Call to greedy decimator
  bool ret = decimate(
    VO, FO,
    cost_and_placement,
    max_faces_stopping_condition(m,orig_m,max_m),
    pre_collapse,
    post_collapse,
    U, G, J, I);
  // Remove phony boundary faces and clean up
  const Eigen::Array<bool,Eigen::Dynamic,1> keep = (J.array()<orig_m);
  const auto keep_i = igl::find(keep);
  G = G(keep_i,igl::placeholders::all).eval();
  J = J(keep_i).eval();
  Eigen::VectorXi _1,I2;
  igl::remove_unreferenced(Eigen::MatrixXd(U),Eigen::MatrixXi(G),U,G,_1,I2);
  I = I(I2).eval();

  assert(tree == nullptr || tree == tree->root());
  delete tree;
  return ret;
}
