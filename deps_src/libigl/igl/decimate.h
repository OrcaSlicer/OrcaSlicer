// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_DECIMATE_H
#define IGL_DECIMATE_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <vector>
#include <set>
namespace igl
{
  // Assumes (V,F) is a manifold mesh (possibly with boundary) Collapses edges
  // until desired number of faces is achieved. This uses default edge cost and
  // merged vertex placement functions {edge length, edge midpoint}.
  //
  // Inputs:
  //   V  #V by dim list of vertex positions
  //   F  #F by 3 list of face indices into V.
  //   max_m  desired number of output faces
  // Outputs:
  //   U  #U by dim list of output vertex posistions (can be same ref as V)
  //   G  #G by 3 list of output face indices into U (can be same ref as G)
  //   J  #G list of indices into F of birth face
  //   I  #U list of indices into V of birth vertices
  // Returns true if m was reached (otherwise #G > m)
  IGL_INLINE bool decimate(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const size_t max_m,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::VectorXi & J,
    Eigen::VectorXi & I);
  // Inputs:
  //   V  #V by dim list of vertex positions
  //   F  #F by 3 list of face indices into V.
  //   max_m  desired number of output faces
  // Outputs:
  //   U  #U by dim list of output vertex posistions (can be same ref as V)
  //   G  #G by 3 list of output face indices into U (can be same ref as G)
  //   J  #G list of indices into F of birth face
  // Returns true if m was reached (otherwise #G > m)
  IGL_INLINE bool decimate(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const size_t max_m,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::VectorXi & J);
  // Assumes a **closed** manifold mesh. See igl::connect_boundary_to_infinity
  // and igl::decimate in decimate.cpp
  // is handling meshes with boundary by connecting all boundary edges with
  // dummy facets to infinity **and** modifying the stopping criteria.
  //
  // Inputs:
  //   cost_and_placement  function computing cost of collapsing an edge and 3d
  //     position where it should be placed:
  //     cost_and_placement(V,F,E,EMAP,EF,EI,cost,placement);
  //   stopping_condition  function returning whether to stop collapsing edges
  //     based on current state. Guaranteed to be called after _successfully_
  //     collapsing edge e removing edges (e,e1,e2) and faces (f1,f2):
  //     bool should_stop =
  //       stopping_condition(V,F,E,EMAP,EF,EI,Q,Qit,C,e,e1,e2,f1,f2);
  IGL_INLINE bool decimate(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const std::function<void(
      const int              /*e*/,
      const Eigen::MatrixXd &/*V*/,
      const Eigen::MatrixXi &/*F*/,
      const Eigen::MatrixXi &/*E*/,
      const Eigen::VectorXi &/*EMAP*/,
      const Eigen::MatrixXi &/*EF*/,
      const Eigen::MatrixXi &/*EI*/,
      double &               /*cost*/,
      Eigen::RowVectorXd &   /*p*/
      )> & cost_and_placement,
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
      const int                                                       ,/*e*/
      const int                                                       ,/*e1*/
      const int                                                       ,/*e2*/
      const int                                                       ,/*f1*/
      const int                                                        /*f2*/
      )> & stopping_condition,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::VectorXi & J,
    Eigen::VectorXi & I);

  // Inputs:
  //   pre_collapse  callback called with index of edge whose collapse is about
  //     to be attempted (see collapse_edge)
  //   post_collapse  callback called with index of edge whose collapse was
  //     just attempted and a flag revealing whether this was successful (see
  //     collapse_edge)
  IGL_INLINE bool decimate(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const std::function<void(
      const int              /*e*/,
      const Eigen::MatrixXd &/*V*/,
      const Eigen::MatrixXi &/*F*/,
      const Eigen::MatrixXi &/*E*/,
      const Eigen::VectorXi &/*EMAP*/,
      const Eigen::MatrixXi &/*EF*/,
      const Eigen::MatrixXi &/*EI*/,
      double &               /*cost*/,
      Eigen::RowVectorXd &   /*p*/
      )> & cost_and_placement,
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
      const int                                                       ,/*e*/
      const int                                                       ,/*e1*/
      const int                                                       ,/*e2*/
      const int                                                       ,/*f1*/
      const int                                                        /*f2*/
      )> & stopping_condition,
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
    Eigen::VectorXi & I);

  // Inputs:
  //   EMAP #F*3 list of indices into E, mapping each directed edge to unique
  //     unique edge in E
  //   EF  #E by 2 list of edge flaps, EF(e,0)=f means e=(i-->j) is the edge of
  //     F(f,:) opposite the vth corner, where EI(e,0)=v. Similarly EF(e,1) "
  //     e=(j->i)
  //   EI  #E by 2 list of edge flap corners (see above).

  IGL_INLINE bool decimate(
    const Eigen::MatrixXd & V,
    const Eigen::MatrixXi & F,
    const std::function<void(
      const int              /*e*/,
      const Eigen::MatrixXd &/*V*/,
      const Eigen::MatrixXi &/*F*/,
      const Eigen::MatrixXi &/*E*/,
      const Eigen::VectorXi &/*EMAP*/,
      const Eigen::MatrixXi &/*EF*/,
      const Eigen::MatrixXi &/*EI*/,
      double &               /*cost*/,
      Eigen::RowVectorXd &   /*p*/
      )> & cost_and_placement,
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
      const int                                                       ,/*e*/
      const int                                                       ,/*e1*/
      const int                                                       ,/*e2*/
      const int                                                       ,/*f1*/
      const int                                                        /*f2*/
      )> & stopping_condition,
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
    const Eigen::MatrixXi & E,
    const Eigen::VectorXi & EMAP,
    const Eigen::MatrixXi & EF,
    const Eigen::MatrixXi & EI,
    Eigen::MatrixXd & U,
    Eigen::MatrixXi & G,
    Eigen::VectorXi & J,
    Eigen::VectorXi & I);

}

#ifndef IGL_STATIC_LIBRARY
#  include "decimate.cpp"
#endif
#endif

