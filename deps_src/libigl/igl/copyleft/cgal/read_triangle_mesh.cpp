// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "read_triangle_mesh.h"
#include "assign.h"
#include "../../read_triangle_mesh.h"

template <typename DerivedV, typename DerivedF>
IGL_INLINE bool igl::copyleft::cgal::read_triangle_mesh(
  const std::string str,
  Eigen::PlainObjectBase<DerivedV>& V,
  Eigen::PlainObjectBase<DerivedF>& F)
{
  Eigen::MatrixXd Vd;
  bool ret = igl::read_triangle_mesh(str,Vd,F);
  if(ret)
  {
    assign(Vd,V);
  }
  return ret;
}
