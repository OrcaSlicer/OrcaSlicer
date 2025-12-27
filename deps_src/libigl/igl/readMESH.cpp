// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "readMESH.h"

template <typename Scalar, typename Index>
IGL_INLINE bool igl::readMESH(
  const std::string mesh_file_name,
  std::vector<std::vector<Scalar > > & V,
  std::vector<std::vector<Index > > & T,
  std::vector<std::vector<Index > > & F)
{
  using namespace std;
  FILE * mesh_file = fopen(mesh_file_name.c_str(),"r");
  if(NULL==mesh_file)
  {
    fprintf(stderr,"IOError: %s could not be opened...",mesh_file_name.c_str());
    return false;
  }
  return igl::readMESH(mesh_file,V,T,F);
}

template <typename Scalar, typename Index>
IGL_INLINE bool igl::readMESH(
  FILE * mesh_file,
  std::vector<std::vector<Scalar > > & V,
  std::vector<std::vector<Index > > & T,
  std::vector<std::vector<Index > > & F)
{
  using namespace std;
#ifndef LINE_MAX
#  define LINE_MAX 2048
#endif
  char line[LINE_MAX];
  bool still_comments;
  V.clear();
  T.clear();
  F.clear();

  // eat comments at beginning of file
  still_comments= true;
  while(still_comments)
  {
    if(fgets(line,LINE_MAX,mesh_file) == NULL)
    {
      fprintf(stderr, "Error: couldn't find start of .mesh file");
      fclose(mesh_file);
      return false;
    }
    still_comments = (line[0] == '#' || line[0] == '\n');
  }
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

  int one = -1;
  if(2 != sscanf(line,"%s %d",str,&one))
  {
    // 1 appears on next line?
    fscanf(mesh_file," %d",&one);
  }
  if(one != 1)
  {
    fprintf(stderr,"Error: second word should be 1 not %d\n",one);
    fclose(mesh_file);
    return false;
  }

  // eat comments
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,mesh_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

  sscanf(line," %s",str);
  // check that third word is Dimension
  if(0!=strcmp(str,"Dimension"))
  {
    fprintf(stderr,"Error: third word should be Dimension not %s\n",str);
    fclose(mesh_file);
    return false;
  }
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

  // eat comments
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,mesh_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

  sscanf(line," %s",str);
  // check that fifth word is Vertices
  if(0!=strcmp(str,"Vertices"))
  {
    fprintf(stderr,"Error: fifth word should be Vertices not %s\n",str);
    fclose(mesh_file);
    return false;
  }

  //fgets(line,LINE_MAX,mesh_file);

  int number_of_vertices;
  if(1 != fscanf(mesh_file," %d",&number_of_vertices) || number_of_vertices > 1000000000)
  {
    fprintf(stderr,"Error: expecting number of vertices less than 10^9...\n");
    fclose(mesh_file);
    return false;
  }
  // allocate space for vertices
  V.resize(number_of_vertices,vector<Scalar>(3,0));
  int extra;
  for(int i = 0;i<number_of_vertices;i++)
  {
    double x,y,z;
    if(4 != fscanf(mesh_file," %lg %lg %lg %d",&x,&y,&z,&extra))
    {
      fprintf(stderr,"Error: expecting vertex position...\n");
      fclose(mesh_file);
      return false;
    }
    V[i][0] = x;
    V[i][1] = y;
    V[i][2] = z;
  }

  // eat comments
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,mesh_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

  sscanf(line," %s",str);
  // check that sixth word is Triangles
  if(0!=strcmp(str,"Triangles"))
  {
    fprintf(stderr,"Error: sixth word should be Triangles not %s\n",str);
    fclose(mesh_file);
    return false;
  }
  int number_of_triangles;
  if(1 != fscanf(mesh_file," %d",&number_of_triangles))
  {
    fprintf(stderr,"Error: expecting number of triangles...\n");
    fclose(mesh_file);
    return false;
  }
  // allocate space for triangles
  F.resize(number_of_triangles,vector<Index>(3));
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
      F[i][j] = tri[j]-1;
    }
  }

  // eat comments
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,mesh_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

  sscanf(line," %s",str);
  // check that sixth word is Triangles
  if(0!=strcmp(str,"Tetrahedra"))
  {
    fprintf(stderr,"Error: seventh word should be Tetrahedra not %s\n",str);
    fclose(mesh_file);
    return false;
  }
  int number_of_tetrahedra;
  if(1 != fscanf(mesh_file," %d",&number_of_tetrahedra))
  {
    fprintf(stderr,"Error: expecting number of tetrahedra...\n");
    fclose(mesh_file);
    return false;
  }
  // allocate space for tetrahedra
  T.resize(number_of_tetrahedra,vector<Index>(4));
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
    T[i][0] = a-1;
    T[i][1] = b-1;
    T[i][2] = c-1;
    T[i][3] = d-1;
  }
  fclose(mesh_file);
  return true;
}

#include <Eigen/Core>
#include "list_to_matrix.h"


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
  bool still_comments;

  // eat comments at beginning of file
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,mesh_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

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
  int one = -1;
  if(2 != sscanf(line,"%s %d",str,&one))
  {
    // 1 appears on next line?
    fscanf(mesh_file," %d",&one);
  }
  if(one != 1)
  {
    fprintf(stderr,"Error: second word should be 1 not %d\n",one);
    fclose(mesh_file);
    return false;
  }

  // eat comments
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,mesh_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

  sscanf(line," %s",str);
  // check that third word is Dimension
  if(0!=strcmp(str,"Dimension"))
  {
    fprintf(stderr,"Error: third word should be Dimension not %s\n",str);
    fclose(mesh_file);
    return false;
  }
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

  // eat comments
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,mesh_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

  sscanf(line," %s",str);
  // check that fifth word is Vertices
  if(0!=strcmp(str,"Vertices"))
  {
    fprintf(stderr,"Error: fifth word should be Vertices not %s\n",str);
    fclose(mesh_file);
    return false;
  }

  //fgets(line,LINE_MAX,mesh_file);

  int number_of_vertices;
  if(1 != fscanf(mesh_file," %d",&number_of_vertices) || number_of_vertices > 1000000000)
  {
    fprintf(stderr,"Error: expecting number of vertices less than 10^9...\n");
    fclose(mesh_file);
    return false;
  }
  // allocate space for vertices
  V.resize(number_of_vertices,3);
  int extra;
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

  // eat comments
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,mesh_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

  sscanf(line," %s",str);
  // check that sixth word is Triangles
  if(0!=strcmp(str,"Triangles"))
  {
    fprintf(stderr,"Error: sixth word should be Triangles not %s\n",str);
    fclose(mesh_file);
    return false;
  }
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

  // eat comments
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,mesh_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

  sscanf(line," %s",str);
  // check that sixth word is Triangles
  if(0!=strcmp(str,"Tetrahedra"))
  {
    fprintf(stderr,"Error: seventh word should be Tetrahedra not %s\n",str);
    fclose(mesh_file);
    return false;
  }
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
  fclose(mesh_file);
  return true;
}
//{
//  std::vector<std::vector<double> > vV,vT,vF;
//  bool success = igl::readMESH(mesh_file_name,vV,vT,vF);
//  if(!success)
//  {
//    // readMESH already printed error message to std err
//    return false;
//  }
//  bool V_rect = igl::list_to_matrix(vV,V);
//  if(!V_rect)
//  {
//    // igl::list_to_matrix(vV,V) already printed error message to std err
//    return false;
//  }
//  bool T_rect = igl::list_to_matrix(vT,T);
//  if(!T_rect)
//  {
//    // igl::list_to_matrix(vT,T) already printed error message to std err
//    return false;
//  }
//  bool F_rect = igl::list_to_matrix(vF,F);
//  if(!F_rect)
//  {
//    // igl::list_to_matrix(vF,F) already printed error message to std err
//    return false;
//  }
//  assert(V.cols() == 3);
//  assert(T.cols() == 4);
//  assert(F.cols() == 3);
//  return true;
//}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template bool igl::readMESH<Eigen::Matrix<float, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
// generated by autoexplicit.sh
template bool igl::readMESH<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(FILE*, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
// generated by autoexplicit.sh
template bool igl::readMESH<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
// generated by autoexplicit.sh
template bool igl::readMESH<Eigen::Matrix<float, -1, 3, 1, -1, 3>, Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<float, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<unsigned int, -1, 3, 1, -1, 3> >&);
// generated by autoexplicit.sh
template bool igl::readMESH<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
// generated by autoexplicit.sh
template bool igl::readMESH<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readMESH<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template bool igl::readMESH<double, int>(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, std::vector<std::vector<double, std::allocator<double> >, std::allocator<std::vector<double, std::allocator<double> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&);
template bool igl::readMESH<Eigen::Matrix<double, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, 3, 1, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 1, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 1, -1, 3> >&);
#endif
