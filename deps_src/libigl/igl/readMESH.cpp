// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "readMESH.h"
#include <Eigen/Core>


template <typename DerivedV, typename DerivedF, typename DerivedT>
IGL_INLINE bool igl::readMESH(
  const std::string mesh_file_name,
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedT>& T,
  Eigen::PlainObjectBase<DerivedF>& F)
{
  using namespace std;
  FILE * mesh_file = fopen(mesh_file_name.c_str(),"r");
  if(NULL==mesh_file)
  {
    fprintf(stderr,"IOError: %s could not be opened...",mesh_file_name.c_str());
    return false;
  }
  return readMESH(mesh_file,V,T,F);
}

template <typename DerivedV, typename DerivedF, typename DerivedT>
IGL_INLINE bool igl::readMESH(
  FILE * mesh_file,
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedT>& T,
  Eigen::PlainObjectBase<DerivedF>& F)
{
  using namespace std;
#ifndef LINE_MAX
#  define LINE_MAX 2048
#endif
  char line[LINE_MAX];

  // eat comments at beginning of file
  const auto eat_comments = [&]()->bool
  {
    bool still_comments= true;
    bool has_line = false;

    const auto is_comment_line = [&](char * line)->bool
    {
      if(line[0] == '#' || line[0] == '\n' || line[0] == '\r')
      {
        return true;
      }
      // or if line is all whitespace
      for(int i = 0;i<LINE_MAX;i++)
      {
        if(line[i] == '\0')
        {
          return true;
        }
        if(!isspace(line[i]))
        {
          return false;
        }
      }
      return false;
    };

    while(still_comments)
    {
      has_line = fgets(line,LINE_MAX,mesh_file) != NULL;

      still_comments = has_line && is_comment_line(line);
    }
    return has_line;
  };
  eat_comments();

  char str[LINE_MAX];
  sscanf(line," %s",str);
  // check that first word is MeshVersionFormatted
  if(0!=strcmp(str,"MeshVersionFormatted"))
  {
    fprintf(stderr,
      "Error: first word should be MeshVersionFormatted not %s\n",str);
    fclose(mesh_file);
    return false;
  }
  int version = -1;
  if(2 != sscanf(line,"%s %d",str,&version)) { fscanf(mesh_file," %d",&version); }
  if(version != 1 && version != 2)
  {
    fprintf(stderr,"Error: second word should be 1 or 2 not %d\n",version);
    fclose(mesh_file);
    return false;
  }

  while(eat_comments())
  {
    sscanf(line," %s",str);
    int extra;
    // check that third word is Dimension
    if(0==strcmp(str,"Dimension"))
    {
      int three = -1;
      if(2 != sscanf(line,"%s %d",str,&three))
      {
        // 1 appears on next line?
        fscanf(mesh_file," %d",&three);
      }
      if(three != 3)
      {
        fprintf(stderr,"Error: only Dimension 3 supported not %d\n",three);
        fclose(mesh_file);
        return false;
      }
    }else if(0==strcmp(str,"Vertices"))
    {
      int number_of_vertices;
      if(1 != fscanf(mesh_file," %d",&number_of_vertices) || number_of_vertices > 1000000000)
      {
        fprintf(stderr,"Error: expecting number of vertices less than 10^9...\n");
        fclose(mesh_file);
        return false;
      }
      // allocate space for vertices
      V.resize(number_of_vertices,3);
      for(int i = 0;i<number_of_vertices;i++)
      {
        double x,y,z;
        if(4 != fscanf(mesh_file," %lg %lg %lg %d",&x,&y,&z,&extra))
        {
          fprintf(stderr,"Error: expecting vertex position...\n");
          fclose(mesh_file);
          return false;
        }
        V(i,0) = x;
        V(i,1) = y;
        V(i,2) = z;
      }
    }else if(0==strcmp(str,"Triangles"))
    {
      int number_of_triangles;
      if(1 != fscanf(mesh_file," %d",&number_of_triangles))
      {
        fprintf(stderr,"Error: expecting number of triangles...\n");
        fclose(mesh_file);
        return false;
      }
      // allocate space for triangles
      F.resize(number_of_triangles,3);
      // triangle indices
      int tri[3];
      for(int i = 0;i<number_of_triangles;i++)
      {
        if(4 != fscanf(mesh_file," %d %d %d %d",&tri[0],&tri[1],&tri[2],&extra))
        {
          printf("Error: expecting triangle indices...\n");
          return false;
        }
        for(int j = 0;j<3;j++)
        {
          F(i,j) = tri[j]-1;
        }
      }
    }else if(0==strcmp(str,"Tetrahedra"))
    {
      int number_of_tetrahedra;
      if(1 != fscanf(mesh_file," %d",&number_of_tetrahedra))
      {
        fprintf(stderr,"Error: expecting number of tetrahedra...\n");
        fclose(mesh_file);
        return false;
      }
      // allocate space for tetrahedra
      T.resize(number_of_tetrahedra,4);
      // tet indices
      int a,b,c,d;
      for(int i = 0;i<number_of_tetrahedra;i++)
      {
        if(5 != fscanf(mesh_file," %d %d %d %d %d",&a,&b,&c,&d,&extra))
        {
          fprintf(stderr,"Error: expecting tetrahedra indices...\n");
          fclose(mesh_file);
          return false;
        }
        T(i,0) = a-1;
        T(i,1) = b-1;
        T(i,2) = c-1;
        T(i,3) = d-1;
      }
    }else if(0==strcmp(str,"Edges"))
    {
      int number_of_edges;
      if(1 != fscanf(mesh_file," %d",&number_of_edges))
      {
        fprintf(stderr,"Error: expecting number of edges...\n");
        fclose(mesh_file);
        return false;
      }
      // allocate space for tetrahedra
      Eigen::MatrixXi E(number_of_edges,2);
      // tet indices
      int a,b;
      for(int i = 0;i<number_of_edges;i++)
      {
        if(3 != fscanf(mesh_file," %d %d %d",&a,&b,&extra))
        {
          fprintf(stderr,"Error: expecting tetrahedra indices...\n");
          fclose(mesh_file);
          return false;
        }
        E(i,0) = a-1;
        E(i,1) = b-1;
      }
    }else if(0==strcmp(str,"End"))
    {
      break;
    }else
    {
      fprintf(stderr,"Error: expecting "
        "Dimension|Triangles|Vertices|Tetrahedra|Edges instead of %s...\n",str);
      fclose(mesh_file);
      return false;
    }
  }

  fclose(mesh_file);
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::readMESH<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readMESH<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readMESH<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readMESH<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template bool igl::readMESH<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
template bool igl::readMESH<Eigen::Matrix<float, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readMESH<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template bool igl::readMESH<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readMESH<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
template bool igl::readMESH<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >&);
#endif
