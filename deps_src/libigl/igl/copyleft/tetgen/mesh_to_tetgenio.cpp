// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "mesh_to_tetgenio.h"

// IGL includes 
#include "../../matrix_to_list.h"

// STL includes
#include <cassert>

IGL_INLINE bool igl::copyleft::tetgen::mesh_to_tetgenio(
  const std::vector<std::vector<REAL > > & V, 
  const std::vector<std::vector<int> > & F, 
  tetgenio & in)
{
  using namespace std;
  // all indices start from 0
  in.firstnumber = 0;

  in.numberofpoints = V.size();
  in.pointlist = new REAL[in.numberofpoints * 3];
  // loop over points
  for(int i = 0; i < (int)V.size(); i++)
  {
    assert(V[i].size() == 3);
    in.pointlist[i*3+0] = V[i][0];
    in.pointlist[i*3+1] = V[i][1];
    in.pointlist[i*3+2] = V[i][2];
  }

  in.numberoffacets = F.size();
  in.facetlist = new tetgenio::facet[in.numberoffacets];
  in.facetmarkerlist = new int[in.numberoffacets];

  // loop over face
  for(int i = 0;i < (int)F.size(); i++)
  {
    in.facetmarkerlist[i] = i;
    tetgenio::facet * f = &in.facetlist[i];
    f->numberofpolygons = 1;
    f->polygonlist = new tetgenio::polygon[f->numberofpolygons];
    f->numberofholes = 0;
    f->holelist = NULL;
    tetgenio::polygon * p = &f->polygonlist[0];
    p->numberofvertices = F[i].size();
    p->vertexlist = new int[p->numberofvertices];
    // loop around face
    for(int j = 0;j < (int)F[i].size(); j++)
    {
      p->vertexlist[j] = F[i][j];
    }
  }
  return true;
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE bool igl::copyleft::tetgen::mesh_to_tetgenio(
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  tetgenio & in)
{
  using namespace std;
  vector<vector<REAL> > vV;
  vector<vector<int> > vF;
  matrix_to_list(V,vV);
  matrix_to_list(F,vF);
  return mesh_to_tetgenio(vV,vF,in);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::copyleft::tetgen::mesh_to_tetgenio<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, tetgenio&);
#endif
