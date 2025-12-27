// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_QSLIM_OPTIMAL_COLLAPSE_EDGE_CALLBACKS_H
#define IGL_QSLIM_OPTIMAL_COLLAPSE_EDGE_CALLBACKS_H
#include "igl_inline.h"
#include <Eigen/Core>
#include <functional>
#include <vector>
#include <tuple>
#include <set>
namespace igl
{

  // Prepare callbacks for decimating edges using the qslim optimal placement
  // metric.
  //
  // Inputs:
  //   E  #E by 2 list of working edges
  //   quadrics  reference to list of working per vertex quadrics 
  //   v1  working variable to maintain end point of collapsed edge
  //   v2  working variable to maintain end point of collapsed edge
  // Outputs
  //   cost_and_placement  callback for evaluating cost of edge collapse and
  //     determining placement of vertex (see collapse_edge)
  //   pre_collapse  callback before edge collapse (see collapse_edge)
  //   post_collapse  callback after edge collapse (see collapse_edge)
  IGL_INLINE void qslim_optimal_collapse_edge_callbacks(
    Eigen::MatrixXi & E,
    std::vector<std::tuple<Eigen::MatrixXd,Eigen::RowVectorXd,double> > & 
      quadrics,
    int & v1,
    int & v2,
    std::function<void(
      const int e,
      const Eigen::MatrixXd &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      const Eigen::VectorXi &,
      const Eigen::MatrixXi &,
      const Eigen::MatrixXi &,
      double &,
      Eigen::RowVectorXd &)> & cost_and_placement,
    std::function<bool(
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
    std::function<void(
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
      )> & post_collapse);
}
#ifndef IGL_STATIC_LIBRARY
#  include "qslim_optimal_collapse_edge_callbacks.cpp"
#endif
#endif
