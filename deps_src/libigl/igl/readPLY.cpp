// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "readPLY.h"
#include "list_to_matrix.h"
#include "ply.h"
#include <iostream>

template <
  typename Vtype,
  typename Ftype,
  typename Ntype,
  typename UVtype>
IGL_INLINE bool igl::readPLY(
  const std::string filename,
  std::vector<std::vector<Vtype> > & V,
  std::vector<std::vector<Ftype> > & F,
  std::vector<std::vector<Ntype> > & N,
  std::vector<std::vector<UVtype> >  & UV)
{
  using namespace std;
  // Largely follows ply2iv.c
  FILE * ply_file = fopen(filename.c_str(),"r");
  if(ply_file == NULL)
  {
    return false;
  }
  return readPLY(ply_file,V,F,N,UV);
}

template <
  typename Vtype,
  typename Ftype,
  typename Ntype,
  typename UVtype>
IGL_INLINE bool igl::readPLY(
  FILE * ply_file,
  std::vector<std::vector<Vtype> > & V,
  std::vector<std::vector<Ftype> > & F,
  std::vector<std::vector<Ntype> > & N,
  std::vector<std::vector<UVtype> >  & UV)
{
  using namespace std;
   typedef struct Vertex {
     double x,y,z;          /* position */
     double nx,ny,nz;         /* surface normal */
     double s,t;              /* texture coordinates */
     void *other_props;       /* other properties */
   } Vertex;

   typedef struct Face {
     unsigned char nverts;    /* number of vertex indices in list */
     int *verts;              /* vertex index list */
     void *other_props;       /* other properties */
   } Face;

  igl::ply::PlyProperty vert_props[] = { /* list of property information for a vertex */
    {"x", PLY_DOUBLE, PLY_DOUBLE, offsetof(Vertex,x), 0, 0, 0, 0},
    {"y", PLY_DOUBLE, PLY_DOUBLE, offsetof(Vertex,y), 0, 0, 0, 0},
    {"z", PLY_DOUBLE, PLY_DOUBLE, offsetof(Vertex,z), 0, 0, 0, 0},
    {"nx", PLY_DOUBLE, PLY_DOUBLE, offsetof(Vertex,nx), 0, 0, 0, 0},
    {"ny", PLY_DOUBLE, PLY_DOUBLE, offsetof(Vertex,ny), 0, 0, 0, 0},
    {"nz", PLY_DOUBLE, PLY_DOUBLE, offsetof(Vertex,nz), 0, 0, 0, 0},
    {"s", PLY_DOUBLE, PLY_DOUBLE, offsetof(Vertex,s), 0, 0, 0, 0},
    {"t", PLY_DOUBLE, PLY_DOUBLE, offsetof(Vertex,t), 0, 0, 0, 0},
  };

  igl::ply::PlyProperty face_props[] = { /* list of property information for a face */
    {"vertex_indices", PLY_INT, PLY_INT, offsetof(Face,verts),
      1, PLY_UCHAR, PLY_UCHAR, offsetof(Face,nverts)},
  };

  int nelems;
  char ** elem_names;
  igl::ply::PlyFile * in_ply = igl::ply::ply_read(ply_file,&nelems,&elem_names);
  if(in_ply==NULL)
  {
    return false;
  }

  bool has_normals = false;
  bool has_texture_coords = false;
  igl::ply::PlyProperty **plist;
  int nprops;
  int elem_count;
  plist = ply_get_element_description (in_ply,"vertex", &elem_count, &nprops);
  int native_binary_type = igl::ply::get_native_binary_type2();
  if (plist != NULL)
  {
    /* set up for getting vertex elements */
    ply_get_property (in_ply,"vertex",&vert_props[0]);
    ply_get_property (in_ply,"vertex",&vert_props[1]);
    ply_get_property (in_ply,"vertex",&vert_props[2]);
    for (int j = 0; j < nprops; j++)
    {
      igl::ply::PlyProperty * prop = plist[j];
      if (igl::ply::equal_strings ("nx", prop->name) 
        || igl::ply::equal_strings ("ny", prop->name)
        || igl::ply::equal_strings ("nz", prop->name))
      {
        ply_get_property (in_ply,"vertex",&vert_props[3]);
        ply_get_property (in_ply,"vertex",&vert_props[4]);
        ply_get_property (in_ply,"vertex",&vert_props[5]);
        has_normals = true;
      }
      if (igl::ply::equal_strings ("s", prop->name) ||
        igl::ply::equal_strings ("t", prop->name))
      {
        ply_get_property(in_ply,"vertex",&vert_props[6]);
        ply_get_property(in_ply,"vertex",&vert_props[7]);
        has_texture_coords = true;
      }
    }
    // Is this call necessary?
    ply_get_other_properties(in_ply,"vertex",
				     offsetof(Vertex,other_props));
    V.resize(elem_count,std::vector<Vtype>(3));
    if(has_normals)
    {
      N.resize(elem_count,std::vector<Ntype>(3));
    }else
    {
      N.resize(0);
    }
    if(has_texture_coords)
    {
      UV.resize(elem_count,std::vector<UVtype>(2));
    }else
    {
      UV.resize(0);
    }
   	
	for(int j = 0;j<elem_count;j++)
    {
      Vertex v;
      ply_get_element_setup(in_ply,"vertex",3,vert_props);
      ply_get_element(in_ply,(void*)&v, &native_binary_type);
      V[j][0] = v.x;
      V[j][1] = v.y;
      V[j][2] = v.z;
      if(has_normals)
      {
        N[j][0] = v.nx;
        N[j][1] = v.ny;
        N[j][2] = v.nz;
      }
      if(has_texture_coords)
      {
        UV[j][0] = v.s;
        UV[j][1] = v.t;
      }
    }
  }
  plist = ply_get_element_description (in_ply,"face", &elem_count, &nprops);
  if (plist != NULL)
  {
    F.resize(elem_count);
    ply_get_property(in_ply,"face",&face_props[0]);
    for (int j = 0; j < elem_count; j++) 
    {
      Face f;
      ply_get_element(in_ply, (void *) &f, &native_binary_type);
      for(size_t c = 0;c<f.nverts;c++)
      {
        F[j].push_back(f.verts[c]);
      }
    }
  }
  ply_close(in_ply);
  return true;
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedN,
  typename DerivedUV>
IGL_INLINE bool igl::readPLY(
  const std::string filename,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F,
  Eigen::PlainObjectBase<DerivedN> & N,
  Eigen::PlainObjectBase<DerivedUV> & UV)
{
  std::vector<std::vector<typename DerivedV::Scalar> > vV;
  std::vector<std::vector<typename DerivedF::Scalar> > vF;
  std::vector<std::vector<typename DerivedN::Scalar> > vN;
  std::vector<std::vector<typename DerivedUV::Scalar> > vUV;
  if(!readPLY(filename,vV,vF,vN,vUV))
  {
    return false;
  }
  return 
    list_to_matrix(vV,V) &&
    list_to_matrix(vF,F) &&
    list_to_matrix(vN,N) &&
    list_to_matrix(vUV,UV);
}

template <
  typename DerivedV,
  typename DerivedF>
IGL_INLINE bool igl::readPLY(
  const std::string filename,
  Eigen::PlainObjectBase<DerivedV> & V,
  Eigen::PlainObjectBase<DerivedF> & F)
{
  Eigen::MatrixXd N,UV;
  return readPLY(filename,V,F,N,UV);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::readPLY<double, int, double, double>(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&);

template bool igl::readPLY<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic>, Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic>, Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic>, Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const, Eigen::PlainObjectBase<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> > &, Eigen::PlainObjectBase<Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic> > &, Eigen::PlainObjectBase<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> > &, Eigen::PlainObjectBase<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> > &);

template bool igl::readPLY<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic>, Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const, Eigen::PlainObjectBase<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> > &, Eigen::PlainObjectBase<Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic> > &);
template bool igl::readPLY<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3> >(std::string, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
#endif
