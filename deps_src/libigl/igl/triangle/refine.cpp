// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2024 Alec Jacobson
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "refine.h"
#include "triangle_header.h"

template <
  typename DerivedV,
  typename DerivedE,
  typename DerivedF,
  typename DerivedV2,
  typename DerivedF2>
IGL_INLINE void igl::triangle::refine(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedE> & E,
  const Eigen::MatrixBase<DerivedF> & F,
  const std::string flags,
  Eigen::PlainObjectBase<DerivedV2> & V2,
  Eigen::PlainObjectBase<DerivedF2> & F2)
{
  using namespace Eigen;
  using namespace Eigen;
  assert(V.cols() == 2);
  assert(F.cols() == 3);
  // Prepare the flags
  std::string full_flags = flags + "rzB" + (E.size()?"p":"");

  typedef Map< Matrix<double,Dynamic,Dynamic,RowMajor> > MapXdr;
  typedef Map< Matrix<int,Dynamic,Dynamic,RowMajor> > MapXir;
 
  // To-do: reduce duplicate code with triangulate.cpp
  // Prepare the input struct
  triangulateio in;
  in.numberofpoints = V.rows();
  in.pointlist = (double*)calloc(V.size(),sizeof(double));
  {
    MapXdr inpl(in.pointlist,V.rows(),V.cols());
    inpl = V.template cast<double>();
  }

  in.numberofpointattributes = 0;
  in.pointmarkerlist = (int*)calloc(V.size(),sizeof(int)) ;
  for(unsigned i=0;i<V.rows();++i) in.pointmarkerlist[i] = 1;

  in.numberoftriangles = F.rows();
  in.trianglelist = (int*)calloc(F.size(),sizeof(int));
  {
    MapXir insl(in.trianglelist,F.rows(),F.cols());
    insl = F.template cast<int>();
  }
  in.numberoftriangleattributes = 0;
  in.triangleattributelist = NULL;

  // Why?
  in.numberofcorners = 3;

  //in.numberofsegments = 0;
  //in.segmentlist = NULL;
  //in.segmentmarkerlist = NULL;

  in.numberofsegments = E.size()?E.rows():0;
  in.segmentlist = (int*)calloc(E.size(),sizeof(int));
  {
    MapXir insl(in.segmentlist,E.rows(),E.cols());
    insl = E.template cast<int>();
  }
  // Empty edge markers (to-do)
  Eigen::VectorXi EM;
  in.segmentmarkerlist = (int*)calloc(E.rows(),sizeof(int));
  for (unsigned i=0;i<E.rows();++i) in.segmentmarkerlist[i] = EM.size()?EM(i):1;

  in.numberofholes = 0;
  in.holelist = NULL;
  in.numberofregions = 0;
 
  // Prepare the output struct
  triangulateio out;
  out.pointlist = NULL;
  out.trianglelist = NULL;
  out.segmentlist = NULL;
  out.segmentmarkerlist = NULL;
  out.pointmarkerlist = NULL;

  // Call triangle
  ::triangulate(const_cast<char*>(full_flags.c_str()), &in, &out, 0);

  // Return the mesh
  V2 = MapXdr(out.pointlist,out.numberofpoints,2).cast<typename DerivedV2::Scalar>();
  F2 = MapXir(out.trianglelist,out.numberoftriangles,3).cast<typename DerivedF2::Scalar>();

  // Cleanup in
  free(in.pointlist);
  free(in.pointmarkerlist);
  free(in.trianglelist );
  // Cleanup out
  free(out.pointlist);
  free(out.trianglelist);
  free(out.segmentlist);
  free(out.segmentmarkerlist);
  free(out.pointmarkerlist);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::triangle::refine<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, std::basic_string<char, std::char_traits<char>, std::allocator<char>>, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>>&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>>&);
#endif
