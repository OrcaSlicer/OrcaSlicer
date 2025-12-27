// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "decimate.h"
#include "collapse_edge.h"
#include "edge_flaps.h"
#include "is_edge_manifold.h"
#include "remove_unreferenced.h"
#include "slice_mask.h"
#include "slice.h"
#include "connect_boundary_to_infinity.h"
#include "max_faces_stopping_condition.h"
#include "shortest_edge_and_midpoint.h"

IGL_INLINE bool igl::decimate(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const size_t max_m,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I)
{
  // Original number of faces
  const int orig_m = F.rows();
  // Tracking number of faces
  int m = F.rows();
  typedef Eigen::MatrixXd DerivedV;
  typedef Eigen::MatrixXi DerivedF;
  DerivedV VO;
  DerivedF FO;
  igl::connect_boundary_to_infinity(V,F,VO,FO);
  // decimate will not work correctly on non-edge-manifold meshes. By extension
  // this includes meshes with non-manifold vertices on the boundary since these
  // will create a non-manifold edge when connected to infinity.
  if(!is_edge_manifold(FO))
  {
    return false;
  }
  bool ret = decimate(
    VO,
    FO,
    shortest_edge_and_midpoint,
    max_faces_stopping_condition(m,orig_m,max_m),
    U,
    G,
    J,
    I);
  const Eigen::Array<bool,Eigen::Dynamic,1> keep = (J.array()<orig_m);
  igl::slice_mask(Eigen::MatrixXi(G),keep,1,G);
  igl::slice_mask(Eigen::VectorXi(J),keep,1,J);
  Eigen::VectorXi _1,I2;
  igl::remove_unreferenced(Eigen::MatrixXd(U),Eigen::MatrixXi(G),U,G,_1,I2);
  igl::slice(Eigen::VectorXi(I),I2,1,I);
  return ret;
}

IGL_INLINE bool igl::decimate(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const size_t max_m,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J)
{
  Eigen::VectorXi I;
  return igl::decimate(V,F,max_m,U,G,J,I);
}

IGL_INLINE bool igl::decimate(
  const Eigen::MatrixXd & OV,
  const Eigen::MatrixXi & OF,
  const std::function<void(
    const int,
    const Eigen::MatrixXd &,
    const Eigen::MatrixXi &,
    const Eigen::MatrixXi &,
    const Eigen::VectorXi &,
    const Eigen::MatrixXi &,
    const Eigen::MatrixXi &,
    double &,
    Eigen::RowVectorXd &)> & cost_and_placement,
  const std::function<bool(
      const Eigen::MatrixXd &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const Eigen::VectorXi &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const std::set<std::pair<double,int> > &,
      const std::vector<std::set<std::pair<double,int> >::iterator > &,
      const Eigen::MatrixXd &,
      const int,
      const int,
      const int,
      const int,
      const int)> & stopping_condition,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I
  )
{
  const auto always_try = [](
    const Eigen::MatrixXd &                                         ,/*V*/
    const Eigen::MatrixXi &                                         ,/*F*/
    const Eigen::MatrixXi &                                         ,/*E*/
    const Eigen::VectorXi &                                         ,/*EMAP*/
    const Eigen::MatrixXi &                                         ,/*EF*/
    const Eigen::MatrixXi &                                         ,/*EI*/
    const std::set<std::pair<double,int> > &                        ,/*Q*/
    const std::vector<std::set<std::pair<double,int> >::iterator > &,/*Qit*/
    const Eigen::MatrixXd &                                         ,/*C*/
    const int                                                        /*e*/
    ) -> bool { return true;};
  const auto never_care = [](
    const Eigen::MatrixXd &                                         ,   /*V*/
    const Eigen::MatrixXi &                                         ,   /*F*/
    const Eigen::MatrixXi &                                         ,   /*E*/
    const Eigen::VectorXi &                                         ,/*EMAP*/
    const Eigen::MatrixXi &                                         ,  /*EF*/
    const Eigen::MatrixXi &                                         ,  /*EI*/
    const std::set<std::pair<double,int> > &                        ,   /*Q*/
    const std::vector<std::set<std::pair<double,int> >::iterator > &, /*Qit*/
    const Eigen::MatrixXd &                                         ,   /*C*/
    const int                                                       ,   /*e*/
    const int                                                       ,  /*e1*/
    const int                                                       ,  /*e2*/
    const int                                                       ,  /*f1*/
    const int                                                       ,  /*f2*/
    const bool                                                  /*collapsed*/
    )-> void { };
  return igl::decimate(
    OV,OF,cost_and_placement,stopping_condition,always_try,never_care,U,G,J,I);
}

IGL_INLINE bool igl::decimate(
  const Eigen::MatrixXd & OV,
  const Eigen::MatrixXi & OF,
  const std::function<void(
    const int,
    const Eigen::MatrixXd &,
    const Eigen::MatrixXi &,
    const Eigen::MatrixXi &,
    const Eigen::VectorXi &,
    const Eigen::MatrixXi &,
    const Eigen::MatrixXi &,
    double &,
    Eigen::RowVectorXd &)> & cost_and_placement,
  const std::function<bool(
      const Eigen::MatrixXd &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const Eigen::VectorXi &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const std::set<std::pair<double,int> > &,
      const std::vector<std::set<std::pair<double,int> >::iterator > &,
      const Eigen::MatrixXd &,
      const int,
      const int,
      const int,
      const int,
      const int)> & stopping_condition,
    const std::function<bool(
      const Eigen::MatrixXd &                                         ,/*V*/
      const Eigen::MatrixXi &                                         ,/*F*/
      const Eigen::MatrixXi &                                         ,/*E*/
      const Eigen::VectorXi &                                         ,/*EMAP*/
      const Eigen::MatrixXi &                                         ,/*EF*/
      const Eigen::MatrixXi &                                         ,/*EI*/
      const std::set<std::pair<double,int> > &                        ,/*Q*/
      const std::vector<std::set<std::pair<double,int> >::iterator > &,/*Qit*/
      const Eigen::MatrixXd &                                         ,/*C*/
      const int                                                        /*e*/
      )> & pre_collapse,
    const std::function<void(
      const Eigen::MatrixXd &                                         ,   /*V*/
      const Eigen::MatrixXi &                                         ,   /*F*/
      const Eigen::MatrixXi &                                         ,   /*E*/
      const Eigen::VectorXi &                                         ,/*EMAP*/
      const Eigen::MatrixXi &                                         ,  /*EF*/
      const Eigen::MatrixXi &                                         ,  /*EI*/
      const std::set<std::pair<double,int> > &                        ,   /*Q*/
      const std::vector<std::set<std::pair<double,int> >::iterator > &, /*Qit*/
      const Eigen::MatrixXd &                                         ,   /*C*/
      const int                                                       ,   /*e*/
      const int                                                       ,  /*e1*/
      const int                                                       ,  /*e2*/
      const int                                                       ,  /*f1*/
      const int                                                       ,  /*f2*/
      const bool                                                  /*collapsed*/
      )> & post_collapse,
  Eigen::MatrixXd & U,
  Eigen::MatrixXi & G,
  Eigen::VectorXi & J,
  Eigen::VectorXi & I
  )
{
  using namespace Eigen;
  using namespace std;
  VectorXi EMAP;
  MatrixXi E,EF,EI;
  edge_flaps(OF,E,EMAP,EF,EI);
  return igl::decimate(
    OV,OF,
    cost_and_placement,stopping_condition,pre_collapse,post_collapse,
    E,EMAP,EF,EI,
    U,G,J,I);
}


IGL_INLINE bool igl::decimate(
  const Eigen::MatrixXd & OV,
  const Eigen::MatrixXi & OF,
  const std::function<void(
    const int,
    const Eigen::MatrixXd &,
    const Eigen::MatrixXi &,
    const Eigen::MatrixXi &,
    const Eigen::VectorXi &,
    const Eigen::MatrixXi &,
    const Eigen::MatrixXi &,
    double &,
    Eigen::RowVectorXd &)> & cost_and_placement,
  const std::function<bool(
      const Eigen::MatrixXd &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const Eigen::VectorXi &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const std::set<std::pair<double,int> > &,
      const std::vector<std::set<std::pair<double,int> >::iterator > &,
      const Eigen::MatrixXd &,
      const int,
      const int,
      const int,
      const int,
      const int)> & stopping_condition,
    const std::function<bool(
      const Eigen::MatrixXd &                                         ,/*V*/
      const Eigen::MatrixXi &                                         ,/*F*/
      const Eigen::MatrixXi &                                         ,/*E*/
      const Eigen::VectorXi &                                         ,/*EMAP*/
      const Eigen::MatrixXi &                                         ,/*EF*/
      const Eigen::MatrixXi &                                         ,/*EI*/
      const std::set<std::pair<double,int> > &                        ,/*Q*/
      const std::vector<std::set<std::pair<double,int> >::iterator > &,/*Qit*/
      const Eigen::MatrixXd &                                         ,/*C*/
      const int                                                        /*e*/
      )> & pre_collapse,
    const std::function<void(
      const Eigen::MatrixXd &                                         ,   /*V*/
      const Eigen::MatrixXi &                                         ,   /*F*/
      const Eigen::MatrixXi &                                         ,   /*E*/
      const Eigen::VectorXi &                                         ,/*EMAP*/
      const Eigen::MatrixXi &                                         ,  /*EF*/
      const Eigen::MatrixXi &                                         ,  /*EI*/
      const std::set<std::pair<double,int> > &                        ,   /*Q*/
      const std::vector<std::set<std::pair<double,int> >::iterator > &, /*Qit*/
      const Eigen::MatrixXd &                                         ,   /*C*/
      const int                                                       ,   /*e*/
      const int                                                       ,  /*e1*/
      const int                                                       ,  /*e2*/
      const int                                                       ,  /*f1*/
      const int                                                       ,  /*f2*/
      const bool                                                  /*collapsed*/
      )> & post_collapse,
  const Eigen::MatrixXi & OE,
  const Eigen::VectorXi & OEMAP,
  const Eigen::MatrixXi & OEF,
  const Eigen::MatrixXi & OEI,
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
  Eigen::MatrixXi E = OE;
  Eigen::VectorXi EMAP = OEMAP;
  Eigen::MatrixXi EF = OEF;
  Eigen::MatrixXi EI = OEI;
  typedef std::set<std::pair<double,int> > PriorityQueue;
  PriorityQueue Q;
  std::vector<PriorityQueue::iterator > Qit;
  Qit.resize(E.rows());
  // If an edge were collapsed, we'd collapse it to these points:
  MatrixXd C(E.rows(),V.cols());
  for(int e = 0;e<E.rows();e++)
  {
    double cost = e;
    RowVectorXd p(1,3);
    cost_and_placement(e,V,F,E,EMAP,EF,EI,cost,p);
    C.row(e) = p;
    Qit[e] = Q.insert(std::pair<double,int>(cost,e)).first;
  }
  int prev_e = -1;
  bool clean_finish = false;

  while(true)
  {
    if(Q.empty())
    {
      break;
    }
    if(Q.begin()->first == std::numeric_limits<double>::infinity())
    {
      // min cost edge is infinite cost
      break;
    }
    int e,e1,e2,f1,f2;
    if(collapse_edge(
       cost_and_placement, pre_collapse, post_collapse,
       V,F,E,EMAP,EF,EI,Q,Qit,C,e,e1,e2,f1,f2))
    {
      if(stopping_condition(V,F,E,EMAP,EF,EI,Q,Qit,C,e,e1,e2,f1,f2))
      {
        clean_finish = true;
        break;
      }
    }else
    {
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
  remove_unreferenced(V,F2,U,G,_1,I);
  return clean_finish;
}
