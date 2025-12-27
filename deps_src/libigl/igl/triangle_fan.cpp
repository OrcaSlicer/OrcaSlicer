// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "triangle_fan.h"
#include "exterior_edges.h"
#include "list_to_matrix.h"

IGL_INLINE void igl::triangle_fan(
  const Eigen::MatrixXi & E,
  Eigen::MatrixXi & cap)
{
  using namespace std;
  using namespace Eigen;

  // Handle lame base case
  if(E.size() == 0)
  {
    cap.resize(0,E.cols()+1);
    return;
  }
  // "Triangulate" aka "close" the E trivially with facets
  // Note: in 2D we need to know if E endpoints are incoming or
  // outgoing (left or right). Thus this will not work.
  assert(E.cols() == 2);
  // Arbitrary starting vertex
  //int s = E(int(((double)rand() / RAND_MAX)*E.rows()),0);
  int s = E(rand()%E.rows(),0);
  vector<vector<int> >  lcap;
  for(int i = 0;i<E.rows();i++)
  {
    // Skip edges incident on s (they would be zero-area)
    if(E(i,0) == s || E(i,1) == s)
    {
      continue;
    }
    vector<int> e(3);
    e[0] = s;
    e[1] = E(i,0);
    e[2] = E(i,1);
    lcap.push_back(e);
  }
  list_to_matrix(lcap,cap);
}

IGL_INLINE Eigen::MatrixXi igl::triangle_fan( const Eigen::MatrixXi & E)
{
  Eigen::MatrixXi cap;
  triangle_fan(E,cap);
  return cap;
}
