// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Daniele Panozzo <daniele.panozzo@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#include "fit_plane.h"
#include <iostream>

IGL_INLINE void igl::fit_plane(
    const Eigen::MatrixXd & V,
    Eigen::RowVector3d & N,
    Eigen::RowVector3d & C)
{
  assert(V.rows()>0);

  Eigen::Vector3d sum = V.colwise().sum();

  Eigen::Vector3d center = sum.array()/(double(V.rows()));

  C = center;

  double sumXX=0.0f,sumXY=0.0f,sumXZ=0.0f;
  double sumYY=0.0f,sumYZ=0.0f;
  double sumZZ=0.0f;

  for(int i=0;i<V.rows();i++)
  {
    double diffX=V(i,0)-center(0);
    double diffY=V(i,1)-center(1);
    double diffZ=V(i,2)-center(2);
    sumXX+=diffX*diffX;
    sumXY+=diffX*diffY;
    sumXZ+=diffX*diffZ;
    sumYY+=diffY*diffY;
    sumYZ+=diffY*diffZ;
    sumZZ+=diffZ*diffZ;
  }

  Eigen::MatrixXd m(3,3);
  m << sumXX,sumXY,sumXZ,
    sumXY,sumYY,sumYZ,
    sumXZ,sumYZ,sumZZ;

  Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> es(m);
  
  N = es.eigenvectors().col(0);
}

#ifdef IGL_STATIC_LIBRARY
#endif



