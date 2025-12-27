// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "random_dir.h"
#include <igl/PI.h>
#include <cmath>

IGL_INLINE Eigen::Vector3d igl::random_dir()
{
  using namespace Eigen;
  double z =  (double)rand() / (double)RAND_MAX*2.0 - 1.0;
  double t =  (double)rand() / (double)RAND_MAX*2.0*PI;
  // http://www.altdevblogaday.com/2012/05/03/generating-uniformly-distributed-points-on-sphere/
  double r = sqrt(1.0-z*z);
  double x = r * cos(t);
  double y = r * sin(t);
  return Vector3d(x,y,z);
}

IGL_INLINE Eigen::MatrixXd igl::random_dir_stratified(const int n)
{
  using namespace Eigen;
  using namespace std;
  const double m = std::floor(sqrt(double(n)));
  MatrixXd N(n,3);
  int row = 0;
  for(int i = 0;i<m;i++)
  {
    const double x = double(i)*1./m;
    for(int j = 0;j<m;j++)
    {
      const double y = double(j)*1./m;
      double z = (x+(1./m)*(double)rand() / (double)RAND_MAX)*2.0 - 1.0;
      double t = (y+(1./m)*(double)rand() / (double)RAND_MAX)*2.0*PI;
      double r = sqrt(1.0-z*z);
      N(row,0) = r * cos(t);
      N(row,1) = r * sin(t);
      N(row,2) = z;
      row++;
    }
  }
  // Finish off with uniform random directions
  for(;row<n;row++)
  {
    N.row(row) = random_dir();
  }
  return N;
}
