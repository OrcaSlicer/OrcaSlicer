// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2019 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "quad_grid.h"
#include "grid.h"

template<
  typename DerivedV,
  typename DerivedQ,
  typename DerivedE>
IGL_INLINE void igl::quad_grid(
  const int nx,
  const int ny,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedQ> & Q,
  Eigen::PlainObjectBase<DerivedE> & E)
{
  grid(Eigen::Vector2i(nx,ny),V);
  return igl::quad_grid(nx,ny,Q,E);
}

template<
  typename DerivedQ,
  typename DerivedE>
IGL_INLINE void igl::quad_grid(
  const int nx,
  const int ny,
  Eigen::PlainObjectBase<DerivedQ> & Q,
  Eigen::PlainObjectBase<DerivedE> & E)
{
  Eigen::MatrixXi I(nx,ny);
  Q.resize(  (nx-1)*(ny-1),4);
  E.resize((nx-1)*ny + (ny-1)*nx,2);
  {
    int v = 0;
    int q = 0;
    int e = 0;
    // Ordered to match igl::grid
    for(int y = 0;y<ny;y++)
    {
      for(int x = 0;x<nx;x++)
      {
        //// Add a vertex
        //V(v,0) = (-1.0) + double(x)/double(nx-1) * (1.0 - (-1.0));
        //V(v,2) = (-1.0) + double(y)/double(ny-1) * (1.0 - (-1.0));
        I(x,y) = v;
        v++;
        // Add a verical edge
        if(y>0)
        {
          E(e,0) = I(x,y);
          E(e,1) = I(x,y-1);
          e++;
        }
        // Add a horizontal edge
        if(x>0)
        {
          E(e,0) = I(x,y);
          E(e,1) = I(x-1,y);
          e++;
        }
        // Add two triangles
        if(x>0 && y>0)
        {
          // -1,0----0,0
          //   |    / |
          //   |   /  |
          //   |  /   |
          //   | /    |
          // -1,-1---0,-1
          Q(q,0) = I(x-0,y-0);
          Q(q,1) = I(x-1,y-0);
          Q(q,2) = I(x-1,y-1);
          Q(q,3) = I(x-0,y-1);
          q++;
          //F(f,2) = I(x-0,y-0);
          //F(f,1) = I(x-1,y-0);
          //F(f,0) = I(x-1,y-1);
          //f++;
          //F(f,2) = I(x-0,y-0);
          //F(f,1) = I(x-1,y-1);
          //F(f,0) = I(x-0,y-1);
          //f++;
        }
      }
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::quad_grid<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(int, int, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
