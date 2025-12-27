// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "writePLY.h"
#include <vector>

#include <igl/ply.h>
#include <vector>

namespace
{
  template <typename Scalar> int ply_type();
  template <> int ply_type<char>(){ return PLY_CHAR; }
  template <> int ply_type<short>(){ return PLY_SHORT; }
  template <> int ply_type<int>(){ return PLY_INT; }
  template <> int ply_type<unsigned char>(){ return PLY_UCHAR; }
  template <> int ply_type<unsigned short>(){ return PLY_SHORT; }
  template <> int ply_type<unsigned int>(){ return PLY_UINT; }
  template <> int ply_type<float>(){ return PLY_FLOAT; }
  template <> int ply_type<double>(){ return PLY_DOUBLE; }
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedN,
  typename DerivedUV>
IGL_INLINE bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedN> & N,
  const Eigen::MatrixBase<DerivedUV> & UV,
  const bool ascii)
{
  // Largely based on obj2ply.c
  typedef typename DerivedV::Scalar VScalar;
  typedef typename DerivedN::Scalar NScalar;
  typedef typename DerivedUV::Scalar UVScalar;
  typedef typename DerivedF::Scalar FScalar;

  typedef struct Vertex
  {
    VScalar x,y,z,w;          /* position */
    NScalar nx,ny,nz;         /* surface normal */
    UVScalar s,t;              /* texture coordinates */
  } Vertex;

  typedef struct Face
  {
    unsigned char nverts;    /* number of vertex indices in list */
    FScalar *verts;              /* vertex index list */
  } Face;

  igl::ply::PlyProperty vert_props[] =
  { /* list of property information for a vertex */
    {"x", ply_type<VScalar>(), ply_type<VScalar>(),offsetof(Vertex,x),0,0,0,0},
    {"y", ply_type<VScalar>(), ply_type<VScalar>(),offsetof(Vertex,y),0,0,0,0},
    {"z", ply_type<VScalar>(), ply_type<VScalar>(),offsetof(Vertex,z),0,0,0,0},
    {"nx",ply_type<NScalar>(), ply_type<NScalar>(),offsetof(Vertex,nx),0,0,0,0},
    {"ny",ply_type<NScalar>(), ply_type<NScalar>(),offsetof(Vertex,ny),0,0,0,0},
    {"nz",ply_type<NScalar>(), ply_type<NScalar>(),offsetof(Vertex,nz),0,0,0,0},
    {"s", ply_type<UVScalar>(),ply_type<UVScalar>(),offsetof(Vertex,s),0,0,0,0},
    {"t", ply_type<UVScalar>(),ply_type<UVScalar>(),offsetof(Vertex,t),0,0,0,0},
  };

  igl::ply::PlyProperty face_props[] =
  { /* list of property information for a face */
    {"vertex_indices", ply_type<FScalar>(), ply_type<FScalar>(), 
      offsetof(Face,verts), 1, PLY_UCHAR, PLY_UCHAR, offsetof(Face,nverts)},
  };
  const bool has_normals = N.rows() > 0;
  const bool has_texture_coords = UV.rows() > 0;
  std::vector<Vertex> vlist(V.rows());
  std::vector<Face> flist(F.rows());
  for(size_t i = 0;i<(size_t)V.rows();i++)
  {
    vlist[i].x = V(i,0);
    vlist[i].y = V(i,1);
    vlist[i].z = V(i,2);
    if(has_normals)
    {
      vlist[i].nx = N(i,0);
      vlist[i].ny = N(i,1);
      vlist[i].nz = N(i,2);
    }
    if(has_texture_coords)
    {
      vlist[i].s = UV(i,0);
      vlist[i].t = UV(i,1);
    }
  }
  for(size_t i = 0;i<(size_t)F.rows();i++)
  {
    flist[i].nverts = F.cols();
    flist[i].verts = new FScalar[F.cols()];
    for(size_t c = 0;c<(size_t)F.cols();c++)
    {
      flist[i].verts[c] = F(i,c);
    }
  }

  const char * elem_names[] = {"vertex","face"};
  FILE * fp = fopen(filename.c_str(),"w");
  if(fp==NULL)
  {
    return false;
  }
  igl::ply::PlyFile * ply = igl::ply::ply_write(fp, 2,elem_names,
      (ascii ? PLY_ASCII : PLY_BINARY_LE));
  if(ply==NULL)
  {
    return false;
  }

  std::vector<igl::ply::PlyProperty> plist;
  plist.push_back(vert_props[0]);
  plist.push_back(vert_props[1]);
  plist.push_back(vert_props[2]);
  if (has_normals)
  {
    plist.push_back(vert_props[3]);
    plist.push_back(vert_props[4]);
    plist.push_back(vert_props[5]);
  }
  if (has_texture_coords)
  {
    plist.push_back(vert_props[6]);
    plist.push_back(vert_props[7]);
  }
  ply_describe_element(ply, "vertex", V.rows(),plist.size(),
    &plist[0]);

  ply_describe_element(ply, "face", F.rows(),1,&face_props[0]);
  ply_header_complete(ply);
  int native_binary_type = igl::ply::get_native_binary_type2();
  ply_put_element_setup(ply, "vertex");
  for(const auto v : vlist)
  {
    ply_put_element(ply, (void *) &v, &native_binary_type);
  }
  ply_put_element_setup(ply, "face");
  for(const auto f : flist)
  {
    ply_put_element(ply, (void *) &f, &native_binary_type);
  }

  ply_close(ply);
  for(size_t i = 0;i<(size_t)F.rows();i++)
  {
    delete[] flist[i].verts;
  }
  return true;
}

template <
  typename DerivedV,
  typename DerivedF>
IGL_INLINE bool igl::writePLY(
  const std::string & filename,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const bool ascii)
{
  Eigen::Matrix<typename DerivedV::Scalar,Eigen::Dynamic,Eigen::Dynamic> N,UV;
  return writePLY(filename,V,F,N,UV,ascii);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::writePLY<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, bool);
// generated by autoexplicit.sh
template bool igl::writePLY<Eigen::Matrix<double, 8, 3, 0, 8, 3>, Eigen::Matrix<int, 12, 3, 0, 12, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, 8, 3, 0, 8, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, 12, 3, 0, 12, 3> > const&, bool);
template bool igl::writePLY<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, bool);
template bool igl::writePLY<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, bool);
template bool igl::writePLY<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> > const&, bool);
#endif
