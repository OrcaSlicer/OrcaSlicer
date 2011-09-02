// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "partition.h"
#include "mat_min.h"

IGL_INLINE void igl::partition(
  const Eigen::MatrixXd & W,
  const int k,
  Eigen::Matrix<int,Eigen::Dynamic,1> & G,
  Eigen::Matrix<int,Eigen::Dynamic,1> & S,
  Eigen::Matrix<double,Eigen::Dynamic,1> & D)
{
  // number of mesh vertices
  int n = W.rows();

  // Resize output
  G.resize(n);
  S.resize(k);

  // "Randomly" choose first seed
  // Pick a vertex farthest from 0
  int s;
  (W.array().square().matrix()).rowwise().sum().maxCoeff(&s);

  S(0) = s;
  // Initialize distance to closest seed
  D = ((W.rowwise() - W.row(s)).array().square()).matrix().rowwise().sum();
  G.setZero();

  // greedily choose the remaining k-1 seeds
  for(int i = 1;i<k;i++)
  {
    // Find maximum in D
    D.maxCoeff(&s);
    S(i) = s;
    // distance to this seed
    Eigen::Matrix<double,Eigen::Dynamic,1> Ds =
      ((W.rowwise() - W.row(s)).array().square()).matrix().rowwise().sum();
    // Concatenation of D and Ds: DDs = [D Ds];
    Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic> DDs;
    // Make space for two columns
    DDs.resize(D.rows(),2);
    DDs.col(0) = D;
    DDs.col(1) = Ds;
    // Update D
    // get minimum of old D and distance to this seed, C == 1 if new distance
    // was smaller
    Eigen::Matrix<int,Eigen::Dynamic,1> C;
    igl::mat_min(DDs,2,D,C);
    G = (C.array() ==0).select(G,i);
  }


}
