// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "triangulate.h"
#ifdef ANSI_DECLARATORS
#  define IGL_PREVIOUSLY_DEFINED_ANSI_DECLARATORS ANSI_DECLARATORS
#  undef ANSI_DECLARATORS
#endif
#ifdef REAL
#  define IGL_PREVIOUSLY_DEFINED_REAL REAL
#  undef REAL
#endif
#ifdef VOID
#  define IGL_PREVIOUSLY_DEFINED_VOID VOID
#  undef VOID
#endif
#define ANSI_DECLARATORS
#define REAL double
#define VOID int

extern "C"
{
#include <triangle.h>
}

#undef ANSI_DECLARATORS
#ifdef IGL_PREVIOUSLY_DEFINED_ANSI_DECLARATORS
#  define ANSI_DECLARATORS IGL_PREVIOUSLY_DEFINED_ANSI_DECLARATORS
#endif

#undef REAL
#ifdef IGL_PREVIOUSLY_DEFINED_REAL
#  define REAL IGL_PREVIOUSLY_DEFINED_REAL
#endif

#undef VOID
#ifdef IGL_PREVIOUSLY_DEFINED_VOID
#  define VOID IGL_PREVIOUSLY_DEFINED_VOID
#endif

template <
 typename DerivedV,
 typename DerivedE,
 typename DerivedH,
 typename DerivedV2,
 typename DerivedF2>
IGL_INLINE void igl::triangle::triangulate(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedE> & E,
  const Eigen::MatrixBase<DerivedH> & H,
  const std::string flags,
  Eigen::PlainObjectBase<DerivedV2> & V2,
  Eigen::PlainObjectBase<DerivedF2> & F2)
{
  Eigen::VectorXi VM,EM,VM2,EM2;
  return triangulate(V,E,H,VM,EM,flags,V2,F2,VM2,EM2);
}

template <
 typename DerivedV,
 typename DerivedE,
 typename DerivedH,
 typename DerivedVM,
 typename DerivedEM,
 typename DerivedV2,
 typename DerivedF2,
 typename DerivedVM2,
 typename DerivedEM2>
IGL_INLINE void igl::triangle::triangulate(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedE> & E,
  const Eigen::MatrixBase<DerivedH> & H,
  const Eigen::MatrixBase<DerivedVM> & VM,
  const Eigen::MatrixBase<DerivedEM> & EM,
  const std::string flags,
  Eigen::PlainObjectBase<DerivedV2> & V2,
  Eigen::PlainObjectBase<DerivedF2> & F2,
  Eigen::PlainObjectBase<DerivedVM2> & VM2,
  Eigen::PlainObjectBase<DerivedEM2> & EM2)
{
  using namespace std;
  using namespace Eigen;

  assert( (VM.size() == 0 || V.rows() == VM.size()) && 
    "Vertex markers must be empty or same size as V");
  assert( (EM.size() == 0 || E.rows() == EM.size()) && 
    "Segment markers must be empty or same size as E");
  assert(V.cols() == 2);
  assert(E.size() == 0 || E.cols() == 2);
  assert(H.size() == 0 || H.cols() == 2);

  // Prepare the flags
  string full_flags = flags + "pz" + (EM.size() || VM.size() ? "" : "B");

  typedef Map< Matrix<double,Dynamic,Dynamic,RowMajor> > MapXdr;
  typedef Map< Matrix<int,Dynamic,Dynamic,RowMajor> > MapXir;

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
  for(unsigned i=0;i<V.rows();++i) in.pointmarkerlist[i] = VM.size()?VM(i):1;

  in.trianglelist = NULL;
  in.numberoftriangles = 0;
  in.numberofcorners = 0;
  in.numberoftriangleattributes = 0;
  in.triangleattributelist = NULL;

  in.numberofsegments = E.size()?E.rows():0;
  in.segmentlist = (int*)calloc(E.size(),sizeof(int));
  {
    MapXir insl(in.segmentlist,E.rows(),E.cols());
    insl = E.template cast<int>();
  }
  in.segmentmarkerlist = (int*)calloc(E.rows(),sizeof(int));
  for (unsigned i=0;i<E.rows();++i) in.segmentmarkerlist[i] = EM.size()?EM(i):1;

  in.numberofholes = H.size()?H.rows():0;
  in.holelist = (double*)calloc(H.size(),sizeof(double));
  {
    MapXdr inhl(in.holelist,H.rows(),H.cols());
    inhl = H.template cast<double>();
  }
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
  if(VM.size())
  {
    VM2 = MapXir(out.pointmarkerlist,out.numberofpoints,1).cast<typename DerivedVM2::Scalar>();
  }
  if(EM.size())
  {
    EM2 = MapXir(out.segmentmarkerlist,out.numberofsegments,1).cast<typename DerivedEM2::Scalar>();
  }

  // Cleanup in
  free(in.pointlist);
  free(in.pointmarkerlist);
  free(in.segmentlist);
  free(in.segmentmarkerlist);
  free(in.holelist);
  // Cleanup out
  free(out.pointlist);
  free(out.trianglelist);
  free(out.segmentlist);
  free(out.segmentmarkerlist);
  free(out.pointmarkerlist);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::triangle::triangulate<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template void igl::triangle::triangulate<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
