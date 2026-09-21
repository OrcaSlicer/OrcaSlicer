// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "decimate.h"
#include "collapse_least_cost_edge.h"
#include "edge_flaps.h"
#include "decimate_trivial_callbacks.h"
#include "AABB.h"
#include "intersection_blocking_collapse_edge_callbacks.h"
#include "is_edge_manifold.h"
#include "remove_unreferenced.h"
#include "placeholders.h"
#include "find.h"
#include "connect_boundary_to_infinity.h"
#include "parallel_for.h"
#include "max_faces_stopping_condition.h"
#include "shortest_edge_and_midpoint.h"
#include "PlainMatrix.h"

IGL_INLINE bool igl::decimate(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const int max_m,
  const bool block_intersections,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I)
{
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
  PlainMatrix<DerivedV> VO;
  PlainMatrix<DerivedF> FO;
  igl::connect_boundary_to_infinity(V,F,VO,FO);
  // These will unfortunately be immediately recomputed in decimate.
  Eigen::VectorXi EMAP;
  Eigen::MatrixXi E,EF,EI;
  edge_flaps(FO,E,EMAP,EF,EI);
  // decimate will not work correctly on non-edge-manifold meshes. By extension
  // this includes meshes with non-manifold vertices on the boundary since these
  // will create a non-manifold edge when connected to infinity.
  {
    Eigen::Array<bool,Eigen::Dynamic,Eigen::Dynamic> BF;
    Eigen::Array<bool,Eigen::Dynamic,1> BE;
    if(!is_edge_manifold(FO,E.rows(),EMAP,BF,BE))
    {
      return false;
    }
  }
  decimate_pre_collapse_callback pre_collapse;
  decimate_post_collapse_callback post_collapse;
  decimate_trivial_callbacks(pre_collapse,post_collapse);
  if(block_intersections)
  {
    igl::intersection_blocking_collapse_edge_callbacks(
      pre_collapse, post_collapse, // These will get copied as needed
      tree,
      pre_collapse, post_collapse);
  }
  bool ret = decimate(
    VO,
    FO,
    shortest_edge_and_midpoint,
    max_faces_stopping_condition(m,orig_m,max_m),
    pre_collapse,
    post_collapse,
    U,
    G,
    J,
    I);
  const Eigen::Array<bool,Eigen::Dynamic,1> keep = (J.array()<orig_m);
  G = G(igl::find(keep),igl::placeholders::all).eval();
  J = J(igl::find(keep)).eval();
  Eigen::VectorXi _1,I2;
  igl::remove_unreferenced(Eigen::MatrixXd(U),Eigen::MatrixXi(G),U,G,_1,I2);
  I = I(I2).eval();
  assert(tree == nullptr || tree == tree->root());
  delete tree;
  return ret;
}

IGL_INLINE bool igl::decimate(
  const Eigen::MatrixXd & OV,
  const Eigen::MatrixXi & OF,
  const decimate_cost_and_placement_callback & cost_and_placement,
  const decimate_stopping_condition_callback & stopping_condition,
  const decimate_pre_collapse_callback       & pre_collapse,
  const decimate_post_collapse_callback      & post_collapse,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I
  )
{
  // Decimate 1
  using namespace Eigen;
  using namespace std;
  // Working copies
  Eigen::MatrixXd V = OV;
  Eigen::MatrixXi F = OF;
  // Why recompute this rather than copy input?
  VectorXi EMAP;
  MatrixXi E,EF,EI;
  edge_flaps(F,E,EMAP,EF,EI);
  {
    Eigen::Array<bool,Eigen::Dynamic,Eigen::Dynamic> BF;
    Eigen::Array<bool,Eigen::Dynamic,1> BE;
    if(!is_edge_manifold(F,E.rows(),EMAP,BF,BE))
    {
      return false;
    }
  }

  igl::min_heap<std::tuple<double,int,int> > Q;
  // Could reserve with https://stackoverflow.com/a/29236236/148668
  Eigen::VectorXi EQ = Eigen::VectorXi::Zero(E.rows());
  // If an edge were collapsed, we'd collapse it to these points:
  MatrixXd C(E.rows(),V.cols());
  // Pushing into a vector then using constructor was slower. Maybe using
  // std::move + make_heap would squeeze out something?
  
  // Separating the cost/placement evaluation from the Q filling is a
  // performance hit for serial but faster if we can parallelize the
  // cost/placement.
  {
    Eigen::VectorXd costs(E.rows());
    igl::parallel_for(E.rows(),[&](const int e)
    {
      double cost = e;
      // RowVectorXd here means that cost_and_placement needs to expect that
      // output parameter type, which makes it hard to use a generic template
      // for cost_and_placement.
      // If we were using more modern C++ then cost_and_placement could return a
      // tuple and could be unpacked into auto [cost,p] =
      // cost_and_placement(...) without much issue.
      RowVectorXd p(1,3);
      cost_and_placement(e,V,F,E,EMAP,EF,EI,cost,p);
      C.row(e) = p;
      costs(e) = cost;
    },
    10000
    );
    for(int e = 0;e<E.rows();e++)
    {
      Q.emplace(costs(e),e,0);
    }
  }


  int prev_e = -1;
  bool clean_finish = false;

  while(true)
  {
    int e,e1,e2,f1,f2;
    if(collapse_least_cost_edge(
      cost_and_placement, pre_collapse, post_collapse,
      V,F,E,EMAP,EF,EI,Q,EQ,C,e,e1,e2,f1,f2))
    {
      if(stopping_condition(V,F,E,EMAP,EF,EI,Q,EQ,C,e,e1,e2,f1,f2))
      {
        clean_finish = true;
        break;
      }
    }else
    {
      if(e == -1)
      {
        // a candidate edge was not even found in Q.
        break;
      }
      if(prev_e == e)
      {
        assert(false && "Edge collapse no progress... bad stopping condition?");
        break;
      }
      // Edge was not collapsed... must have been invalid. collapse_edge should
      // have updated its cost to inf... continue
    }
    prev_e = e;
  }
  // remove all IGL_COLLAPSE_EDGE_NULL faces
  MatrixXi F2(F.rows(),3);
  J.resize(F.rows());
  int m = 0;
  for(int f = 0;f<F.rows();f++)
  {
    if(
      F(f,0) != IGL_COLLAPSE_EDGE_NULL || 
      F(f,1) != IGL_COLLAPSE_EDGE_NULL || 
      F(f,2) != IGL_COLLAPSE_EDGE_NULL)
    {
      F2.row(m) = F.row(f);
      J(m) = f;
      m++;
    }
  }
  F2.conservativeResize(m,F2.cols());
  J.conservativeResize(m);
  VectorXi _1;
  igl::remove_unreferenced(V,F2,U,G,_1,I);
  return clean_finish;
}

