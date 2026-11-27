// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "qslim_optimal_collapse_edge_callbacks.h"
#include "quadric_binary_plus_operator.h"
#include <Eigen/LU>

IGL_INLINE void igl::qslim_optimal_collapse_edge_callbacks(
  Eigen::MatrixXi & /*E*/,
  std::vector<std::tuple<Eigen::MatrixXd,Eigen::RowVectorXd,double> > & 
    quadrics,
  int & v1,
  int & v2,
  decimate_cost_and_placement_callback & cost_and_placement,
  decimate_pre_collapse_callback       & pre_collapse,
  decimate_post_collapse_callback      & post_collapse)
{
  typedef std::tuple<Eigen::MatrixXd,Eigen::RowVectorXd,double> Quadric;
  cost_and_placement = [&quadrics](
    const int e,
    const Eigen::MatrixXd & /*V*/,
    const Eigen::MatrixXi & /*F*/,
    const Eigen::MatrixXi & E,
    const Eigen::VectorXi & /*EMAP*/,
    const Eigen::MatrixXi & /*EF*/,
    const Eigen::MatrixXi & /*EI*/,
    double & cost,
    Eigen::RowVectorXd & p)
  {
    // Combined quadric
    Quadric quadric_p;
    quadric_p = quadrics[E(e,0)] + quadrics[E(e,1)];
    // Quadric: p'Ap + 2b'p + c
    // optimal point: Ap = -b, or rather because we have row vectors: pA=-b
    const auto & A = std::get<0>(quadric_p);
    const auto & b = std::get<1>(quadric_p);
    const auto & c = std::get<2>(quadric_p);
    if(b.array().isInf().any())
    {
      cost = std::numeric_limits<double>::infinity();
      p.resizeLike(b);
      p.setConstant(std::numeric_limits<double>::quiet_NaN());
    }else
    {
      p = -b*A.inverse();
      cost = p.dot(p*A) + 2*p.dot(b) + c;
    }

    // Force infs and nans to infinity
    if(std::isinf(cost) || cost!=cost)
    {
      cost = std::numeric_limits<double>::infinity();
      // Prevent NaNs. Actually NaNs might be useful for debugging.
      p.setConstant(0);
    }
  };
  // Remember endpoints
  pre_collapse = [&v1,&v2](
    const Eigen::MatrixXd &                             ,/*V*/
    const Eigen::MatrixXi &                             ,/*F*/
    const Eigen::MatrixXi & E                           ,
    const Eigen::VectorXi &                             ,/*EMAP*/
    const Eigen::MatrixXi &                             ,/*EF*/
    const Eigen::MatrixXi &                             ,/*EI*/
    const igl::min_heap< std::tuple<double,int,int> > & ,/*Q*/
    const Eigen::VectorXi &                             ,/*EQ*/
    const Eigen::MatrixXd &                             ,/*C*/
    const int e)->bool
  {
    v1 = E(e,0);
    v2 = E(e,1);
    return true;
  };
  // update quadric
  post_collapse = [&v1,&v2,&quadrics](
      const Eigen::MatrixXd &                             ,   /*V*/
      const Eigen::MatrixXi &                             ,   /*F*/
      const Eigen::MatrixXi &                             ,   /*E*/
      const Eigen::VectorXi &                             ,/*EMAP*/
      const Eigen::MatrixXi &                             ,  /*EF*/
      const Eigen::MatrixXi &                             ,  /*EI*/
      const igl::min_heap< std::tuple<double,int,int> > & ,/*Q*/
      const Eigen::VectorXi &                             ,/*EQ*/
      const Eigen::MatrixXd &                             ,   /*C*/
      const int                                           ,   /*e*/
      const int                                           ,  /*e1*/
      const int                                           ,  /*e2*/
      const int                                           ,  /*f1*/
      const int                                           ,  /*f2*/
      const bool                                          collapsed
      )->void
  {
    if(collapsed)
    {
      quadrics[v1<v2?v1:v2] = quadrics[v1] + quadrics[v2];
    }
  };
}

