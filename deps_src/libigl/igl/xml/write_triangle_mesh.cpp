// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "write_triangle_mesh.h"
#include "../write_triangle_mesh.h"
#include "../pathinfo.h"
#include "writeDAE.h"

template <typename DerivedV, typename DerivedF>
IGL_INLINE bool igl::xml::write_triangle_mesh(
  const std::string str,
  const Eigen::PlainObjectBase<DerivedV>& V,
  const Eigen::PlainObjectBase<DerivedF>& F,
  const bool ascii)
{
  using namespace std;
  // dirname, basename, extension and filename
  string d,b,e,f;
  pathinfo(str,d,b,e,f);
  // Convert extension to lower case
  std::transform(e.begin(), e.end(), e.begin(), ::tolower);
  if(e == "dae")
  {
    return writeDAE(str,V,F);
  }else
  {
    return igl::write_triangle_mesh(str,V,F,ascii);
  }
}
