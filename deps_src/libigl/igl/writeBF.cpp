// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "writeBF.h"
#include <fstream>
#include <cassert>
template < 
  typename DerivedWI,
  typename DerivedP,
  typename DerivedO>
IGL_INLINE bool igl::writeBF(
  const std::string & filename,
  const Eigen::PlainObjectBase<DerivedWI> & WI,
  const Eigen::PlainObjectBase<DerivedP> & P,
  const Eigen::PlainObjectBase<DerivedO> & O)
{
  using namespace Eigen;
  using namespace std;
  const int n = WI.rows();
  assert(n == WI.rows() && "WI must have n rows");
  assert(n == P.rows()  && "P must have n rows");
  assert(n == O.rows()  && "O must have n rows");
  MatrixXd WIPO(n,1+1+3);
  for(int i = 0;i<n;i++)
  {
    WIPO(i,0) = WI(i);
    WIPO(i,1) = P(i);
    WIPO(i,2+0) = O(i,0);
    WIPO(i,2+1) = O(i,1);
    WIPO(i,2+2) = O(i,2);
  }
  ofstream s(filename);
  if(!s.is_open())
  {
    fprintf(stderr,"IOError: writeBF() could not open %s\n",filename.c_str());
    return false;
  }
  s<<
    WIPO.format(IOFormat(FullPrecision,DontAlignCols," ","\n","","","","\n"));
  return true;
}

#ifdef IGL_STATIC_LIBRARY
template bool igl::writeBF<Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(std::basic_string<char, std::char_traits<char>, std::allocator<char> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&);
#endif
