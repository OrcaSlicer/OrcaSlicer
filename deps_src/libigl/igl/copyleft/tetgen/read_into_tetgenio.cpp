// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "read_into_tetgenio.h"
#include "mesh_to_tetgenio.h"

// IGL includes
#include "../../pathinfo.h"
#ifndef IGL_NO_EIGEN
#  define IGL_NO_EIGEN_WAS_NOT_ALREADY_DEFINED
#  define IGL_NO_EIGEN
#endif 
// Include igl headers without including Eigen
#include "../../readOBJ.h"
#ifdef IGL_NO_EIGEN_WAS_NOT_ALREADY_DEFINED
#  undef IGL_NO_EIGEN
#endif

// STL includes
#include <algorithm>
#include <iostream>
#include <vector>

IGL_INLINE bool igl::copyleft::tetgen::read_into_tetgenio(
  const std::string & path,
  tetgenio & in)
{
  using namespace std;
  // get file extension
  string dirname,basename,ext,filename;
  pathinfo(path,dirname,basename,ext,filename);
  // convert to lower case for easy comparison
  transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  bool success = false;

  char basename_char[1024];
  strcpy(basename_char,basename.c_str());

  if(ext == "obj")
  {
    // read obj into vertex list and face list
    vector<vector<REAL> > V,TC,N;
    vector<vector<int>  > F,FTC,FN;
    success = readOBJ(path,V,TC,N,F,FTC,FN);
    success &= mesh_to_tetgenio(V,F,in);
  }else if(ext == "off")
  {
    success = in.load_off(basename_char);
  }else if(ext == "node")
  {
    success = in.load_node(basename_char);
  }else 
  {
    if(ext.length() > 0)
    {
      cerr<<"^read_into_tetgenio Warning: Unsupported extension ("<<ext<<
        "): try to load as basename..."<<endl;
    }
    // This changed as of (the so far unreleased) tetgen 1.5
    //success = in.load_tetmesh(basename_char);
    //int object = tetgenbehavior::NODES;
    //if(ext == "mesh")
    //{
    //  object = tetgenbehavior::MEDIT;
    //}
    success = in.load_tetmesh(basename_char,!tetgenbehavior::MEDIT);
  }

  return success;
}
