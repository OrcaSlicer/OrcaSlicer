// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "readTGF.h"

#include <cstdio>

IGL_INLINE bool igl::readTGF(
  const std::string tgf_filename,
  std::vector<std::vector<double> > & C,
  std::vector<std::vector<int> > & E,
  std::vector<int> & P,
  std::vector<std::vector<int> > & BE,
  std::vector<std::vector<int> > & CE,
  std::vector<std::vector<int> > & PE)
{
  using namespace std;
  // clear output
  C.clear();
  E.clear();
  P.clear();
  BE.clear();
  CE.clear();
  PE.clear();

  FILE * tgf_file = fopen(tgf_filename.c_str(),"r");                                       
  if(NULL==tgf_file)
  {
    printf("IOError: %s could not be opened\n",tgf_filename.c_str());
    return false;  
  }

  bool reading_vertices = true;
  bool reading_edges = true;
  const int MAX_LINE_LENGTH = 500;
  char line[MAX_LINE_LENGTH];
  // read until seeing end of file
  while(fgets(line,MAX_LINE_LENGTH,tgf_file)!=NULL)
  {
    // comment signifies end of vertices, next line is start of edges
    if(line[0] == '#')
    {
      if(reading_vertices)
      {
        reading_vertices = false;
        reading_edges = true;
      }else if(reading_edges)
      {
        reading_edges = false;
      }
    // process vertex line
    }else if(reading_vertices)
    {
      int index;
      vector<double> position(3);
      int count = 
        sscanf(line,"%d %lg %lg %lg",
          &index,
          &position[0],
          &position[1],
          &position[2]);
      if(count != 4)
      {
        fprintf(stderr,"Error: readTGF.h: bad format in vertex line\n");
        fclose(tgf_file);
        return false;
      }
      // index is ignored since vertices must already be in order
      C.push_back(position);
    }else if(reading_edges)
    {
      vector<int> edge(2);
      int is_BE = 0;
      int is_PE = 0;
      int is_CE = 0;
      int count = sscanf(line,"%d %d %d %d %d\n",
        &edge[0],
        &edge[1],
        &is_BE,
        &is_PE,
        &is_CE);
      if(count<2)
      {
        fprintf(stderr,"Error: readTGF.h: bad format in edge line\n");
        fclose(tgf_file);
        return false;
      }
      // .tgf is one indexed
      edge[0]--;
      edge[1]--;
      E.push_back(edge);
      if(is_BE == 1)
      {
        BE.push_back(edge);
      }
      if(is_PE == 1)
      {
        // PE should index P
        fprintf(stderr,
          "Warning: readTGF.h found pseudo edges but does not support "
          "them\n");
      }
      if(is_CE == 1)
      {
        // CE should index P
        fprintf(stderr,
          "Warning: readTGF.h found cage edges but does not support them\n");
      }
    }else
    {
      // ignore faces
    }
  }
  fclose(tgf_file);
  // Construct P, indices not in BE
  for(int i = 0;i<(int)C.size();i++)
  {
    bool in_edge = false;
    for(int j = 0;j<(int)BE.size();j++)
    {
      if(i == BE[j][0] || i == BE[j][1])
      {
        in_edge = true;
        break;
      }
    }
    if(!in_edge)
    {
      P.push_back(i);
    }
  }
  return true;
}

#ifndef IGL_NO_EIGEN
#include "list_to_matrix.h"

IGL_INLINE bool igl::readTGF(
  const std::string tgf_filename,
  Eigen::MatrixXd & C,
  Eigen::MatrixXi & E,
  Eigen::VectorXi & P,
  Eigen::MatrixXi & BE,
  Eigen::MatrixXi & CE,
  Eigen::MatrixXi & PE)
{
  std::vector<std::vector<double> > vC;
  std::vector<std::vector<int> > vE;
  std::vector<int> vP;
  std::vector<std::vector<int> > vBE;
  std::vector<std::vector<int> > vCE;
  std::vector<std::vector<int> > vPE;
  bool success = readTGF(tgf_filename,vC,vE,vP,vBE,vCE,vPE);
  if(!success)
  {
    return false;
  }

  if(!list_to_matrix(vC,C))
  {
    return false;
  }
  if(!list_to_matrix(vE,E))
  {
    return false;
  }
  if(!list_to_matrix(vP,P))
  {
    return false;
  }
  if(!list_to_matrix(vBE,BE))
  {
    return false;
  }
  if(!list_to_matrix(vCE,CE))
  {
    return false;
  }
  if(!list_to_matrix(vPE,PE))
  {
    return false;
  }

  return true;
}

IGL_INLINE bool igl::readTGF(
  const std::string tgf_filename,
  Eigen::MatrixXd & C,
  Eigen::MatrixXi & E)
{
  Eigen::VectorXi P;
  Eigen::MatrixXi BE,CE,PE;
  return readTGF(tgf_filename,C,E,P,BE,CE,PE);
}
#endif
