// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "readNODE.h"
#include "matrix_to_list.h"
#include <stdio.h>

template <typename Scalar, typename Index>
IGL_INLINE bool igl::readNODE(
  const std::string node_file_name,
  std::vector<std::vector<Scalar > > & V,
  std::vector<std::vector<Index > > & I)
{
  // TODO: should be templated
  Eigen::MatrixXd mV;
  Eigen::MatrixXi mI;
  if(igl::readNODE(node_file_name,mV,mI))
  {
    matrix_to_list(mV,V);
    matrix_to_list(mI,I);
    return true;
  }else
  {
    return false;
  }
}

template <typename DerivedV, typename DerivedI>
IGL_INLINE bool igl::readNODE(
  const std::string node_file_name,
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedI>& I)
{
  using namespace std;
  FILE * node_file = fopen(node_file_name.c_str(),"r");
  if(NULL==node_file)
  {
    fprintf(stderr,"readNODE: IOError: %s could not be opened...\n",
      node_file_name.c_str());
    return false;
  }
#ifndef LINE_MAX
#  define LINE_MAX 2048
#endif
  char line[LINE_MAX];
  bool still_comments;

  // eat comments at beginning of file
  still_comments= true;
  while(still_comments)
  {
    fgets(line,LINE_MAX,node_file);
    still_comments = (line[0] == '#' || line[0] == '\n');
  }

  // Read header
  // n  number of points
  // dim  dimension
  // num_attr  number of attributes
  // num_bm  number of boundary markers
  int n,dim,num_attr,num_bm;
  int head_count = sscanf(line,"%d %d %d %d", &n, &dim, &num_attr, &num_bm);
  if(head_count!=4)
  {
    fprintf(stderr,"readNODE: Error: incorrect header in %s...\n",
      node_file_name.c_str());
    fclose(node_file);
    return false;
  }
  if(num_attr)
  {
    fprintf(stderr,"readNODE: Error: %d attributes found in %s. "
      "Attributes are not supported...\n",
      num_attr,
      node_file_name.c_str());
    fclose(node_file);
    return false;
  }
  // Just quietly ignore boundary markers
  //if(num_bm)
  //{
  //  fprintf(stderr,"readNODE: Warning: %d boundary markers found in %s. "
  //    "Boundary markers are ignored...\n",
  //    num_bm,
  //    node_file_name.c_str());
  //}

  // resize output
  V.resize(n,dim);
  I.resize(n,1);

  int line_no = 0;
  int p = 0;
  while (fgets(line, LINE_MAX, node_file) != NULL) 
  {
    line_no++;
    // Skip comments and blank lines
    if(line[0] == '#' || line[0] == '\n')
    {
      continue;
    }
    char * l = line;
    int offset;

    if(sscanf(l,"%d%n",&I(p),&offset) != 1)
    {
      fprintf(stderr,"readNODE Error: bad index (%d) in %s...\n",
        line_no,
        node_file_name.c_str());
      fclose(node_file);
      return false;
    }
    // adjust offset
    l += offset;

    // Read coordinates
    for(int d = 0;d<dim;d++)
    {
      if(sscanf(l,"%lf%n",&V(p,d),&offset) != 1)
      {
        fprintf(stderr,"readNODE Error: bad coordinates (%d) in %s...\n",
          line_no,
          node_file_name.c_str());
        fclose(node_file);
        return false;
      }
      // adjust offset
      l += offset;
    }
    // Read boundary markers
    for(int b = 0;b<num_bm;b++)
    {
      int dummy;
      if(sscanf(l,"%d%n",&dummy,&offset) != 1)
      {
        fprintf(stderr,"readNODE Error: bad boundary markers (%d) in %s...\n",
          line_no,
          node_file_name.c_str());
        fclose(node_file);
        return false;
      }
      // adjust offset
      l += offset;
    }
    p++;
  }

  assert(p == V.rows());

  fclose(node_file);
  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::readNODE<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> >, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
