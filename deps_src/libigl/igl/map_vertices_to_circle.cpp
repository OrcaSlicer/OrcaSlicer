// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Stefan Brugger <stefanbrugger@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "map_vertices_to_circle.h"
#include "PI.h"

IGL_INLINE void igl::map_vertices_to_circle(
  const Eigen::MatrixXd& V,
  const Eigen::VectorXi& bnd,
  Eigen::MatrixXd& UV)
{
  // Get sorted list of boundary vertices
  std::vector<int> interior,map_ij;
  map_ij.resize(V.rows());

  std::vector<bool> isOnBnd(V.rows(),false);
  for (int i = 0; i < bnd.size(); i++)
  {
    isOnBnd[bnd[i]] = true;
    map_ij[bnd[i]] = i;
  }

  for (int i = 0; i < (int)isOnBnd.size(); i++)
  {
    if (!isOnBnd[i])
    {
      map_ij[i] = interior.size();
      interior.push_back(i);
    }
  }

  // Map boundary to unit circle
  std::vector<double> len(bnd.size());
  len[0] = 0.;

  for (int i = 1; i < bnd.size(); i++)
  {
    len[i] = len[i-1] + (V.row(bnd[i-1]) - V.row(bnd[i])).norm();
  }
  double total_len = len[len.size()-1] + (V.row(bnd[0]) - V.row(bnd[bnd.size()-1])).norm();

  UV.resize(bnd.size(),2);
  for (int i = 0; i < bnd.size(); i++)
  {
    double frac = len[i] * 2. * igl::PI / total_len;
    UV.row(map_ij[bnd[i]]) << cos(frac), sin(frac);
  }

}
