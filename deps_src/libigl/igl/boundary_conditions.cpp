// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "boundary_conditions.h"

#include "verbose.h"
#include "EPS.h"
#include "project_to_line.h"

#include <vector>
#include <map>
#include <iostream>

template <
  typename DerivedV,
  typename DerivedEle,
  typename DerivedC,
  typename DerivedP,
  typename DerivedBE,
  typename DerivedCE,
  typename DerivedCF,
  typename Derivedb,
  typename Derivedbc>
IGL_INLINE bool igl::boundary_conditions(
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedEle> & Ele,
  const Eigen::MatrixBase<DerivedC> & C,
  const Eigen::MatrixBase<DerivedP> & P,
  const Eigen::MatrixBase<DerivedBE> & BE,
  const Eigen::MatrixBase<DerivedCE> & CE,
  const Eigen::MatrixBase<DerivedCF> & CF,
  Eigen::PlainObjectBase<Derivedb> & b,
  Eigen::PlainObjectBase<Derivedbc> & bc)
{
  using namespace Eigen;
  using namespace std;

  if(P.size()+BE.rows() == 0)
  {
    verbose("^%s: Error: no handles found\n",__FUNCTION__);
    return false;
  }

  vector<int> bci;
  vector<int> bcj;
  vector<double> bcv;

  // loop over points
  for(int p = 0;p<P.size();p++)
  {
    VectorXd pos = C.row(P(p));
    // loop over domain vertices
    for(int i = 0;i<V.rows();i++)
    {
      // Find samples just on pos
      //Vec3 vi(V(i,0),V(i,1),V(i,2));
      // EIGEN GOTCHA:
      // double sqrd = (V.row(i)-pos).array().pow(2).sum();
      // Must first store in temporary
      VectorXd vi = V.row(i);
      double sqrd = (vi-pos).squaredNorm();
      if(sqrd <= FLOAT_EPS)
      {
        //cout<<"sum((["<<
        //  V(i,0)<<" "<<
        //  V(i,1)<<" "<<
        //  V(i,2)<<"] - ["<<
        //  pos(0)<<" "<<
        //  pos(1)<<" "<<
        //  pos(2)<<"]).^2) = "<<sqrd<<endl;
        bci.push_back(i);
        bcj.push_back(p);
        bcv.push_back(1.0);
      }
    }
  }

  // loop over bone edges
  for(int e = 0;e<BE.rows();e++)
  {
    // loop over domain vertices
    for(int i = 0;i<V.rows();i++)
    {
      // Find samples from tip up to tail
      VectorXd tip = C.row(BE(e,0));
      VectorXd tail = C.row(BE(e,1));
      // Compute parameter along bone and squared distance
      double t,sqrd;
      project_to_line(
          V(i,0),V(i,1),V(i,2),
          tip(0),tip(1),tip(2),
          tail(0),tail(1),tail(2),
          t,sqrd);
      if(t>=-FLOAT_EPS && t<=(1.0f+FLOAT_EPS) && sqrd<=FLOAT_EPS)
      {
        bci.push_back(i);
        bcj.push_back(P.size()+e);
        bcv.push_back(1.0);
      }
    }
  }

  // loop over cage edges
  for(int e = 0;e<CE.rows();e++)
  {
    // loop over domain vertices
    for(int i = 0;i<V.rows();i++)
    {
      // Find samples from tip up to tail
      VectorXd tip = C.row(P(CE(e,0)));
      VectorXd tail = C.row(P(CE(e,1)));
      // Compute parameter along bone and squared distance
      double t,sqrd;
      project_to_line(
          V(i,0),V(i,1),V(i,2),
          tip(0),tip(1),tip(2),
          tail(0),tail(1),tail(2),
          t,sqrd);
      if(t>=-FLOAT_EPS && t<=(1.0f+FLOAT_EPS) && sqrd<=FLOAT_EPS)
      {
        bci.push_back(i);
        bcj.push_back(CE(e,0));
        bcv.push_back(1.0-t);
        bci.push_back(i);
        bcj.push_back(CE(e,1));
        bcv.push_back(t);
      }
    }
  }

  std::vector<bool> vertices_marked(V.rows(), false);
  // loop over cage faces
  for(int f = 0;f<CF.rows();f++)
  {
    Vector3d v_0 = C.row(P(CF(f, 0)));
    Vector3d v_1 = C.row(P(CF(f, 1)));
    Vector3d v_2 = C.row(P(CF(f, 2)));
    Vector3d n = (v_1 - v_0).cross(v_2 - v_1);
    n.normalize();
    // loop over domain vertices
    for (int i = 0;i<V.rows();i++)
    {
      // ensure each vertex is associated with only one face
      if (vertices_marked[i])
      {
          continue;
      }
      Vector3d point = V.row(i);
      Vector3d v = point - v_0;
      double dist = abs(v.dot(n));
      if (dist <= 1.e-1f)
      {
        //barycentric coordinates
        Vector3d vec_0 = v_1 - v_0, vec_1 = v_2 - v_0, vec_2 = point - v_0;
        double d00 = vec_0.dot(vec_0);
        double d01 = vec_0.dot(vec_1);
        double d11 = vec_1.dot(vec_1);
        double d20 = vec_2.dot(vec_0);
        double d21 = vec_2.dot(vec_1);
        double denom = d00 * d11 - d01 * d01;
        double v = (d11 * d20 - d01 * d21) / denom;
        double w = (d00 * d21 - d01 * d20) / denom;
        double u = 1.0 - v - w;

        if (u>=0. && u<=1.0 && v>=0. && v<=1.0 && w >=0. && w<=1.0)
        {
          vertices_marked[i] = true;
          bci.push_back(i);
          bcj.push_back(CF(f, 0));
          bcv.push_back(u);
          bci.push_back(i);
          bcj.push_back(CF(f, 1));
          bcv.push_back(v);
          bci.push_back(i);
          bcj.push_back(CF(f, 2));
          bcv.push_back(w);
        }
      }
    }
  }

  // find unique boundary indices
  vector<int> vb = bci;
  sort(vb.begin(),vb.end());
  vb.erase(unique(vb.begin(), vb.end()), vb.end());

  b.resize(vb.size());
  bc = MatrixXd::Zero(vb.size(),P.size()+BE.rows());
  // Map from boundary index to index in boundary
  map<int,int> bim;
  int i = 0;
  // Also fill in b
  for(vector<int>::iterator bit = vb.begin();bit != vb.end();bit++)
  {
    b(i) = *bit;
    bim[*bit] = i;
    i++;
  }

  // Build BC
  for(i = 0;i < (int)bci.size();i++)
  {
    assert(bim.find(bci[i]) != bim.end());
    bc(bim[bci[i]],bcj[i]) = bcv[i];
  }

  // Normalize across rows so that conditions sum to one
  for(i = 0;i<bc.rows();i++)
  {
    double sum = bc.row(i).sum();
    assert(sum != 0 && "Some boundary vertex getting all zero BCs");
    bc.row(i).array() /= sum;
  }

  if(bc.size() == 0)
  {
    verbose("^%s: Error: boundary conditions are empty.\n",__FUNCTION__);
    return false;
  }

  // If there's only a single boundary condition, the following tests
  // are overzealous.
  if(bc.cols() == 1)
  {
    // If there is only one weight function,
    // then we expect that there is only one handle.
    assert(P.rows() + BE.rows() == 1);
    return true;
  }

  // Check that every Weight function has at least one boundary value of 1 and
  // one value of 0
  for(i = 0;i<bc.cols();i++)
  {
    double min_abs_c = bc.col(i).array().abs().minCoeff();
    double max_c = bc.col(i).maxCoeff();
    if(min_abs_c > FLOAT_EPS)
    {
      verbose("^%s: Error: handle %d does not receive 0 weight\n",__FUNCTION__,i);
      return false;
    }
    if(max_c< (1-FLOAT_EPS))
    {
      verbose("^%s: Error: handle %d does not receive 1 weight\n",__FUNCTION__,i);
      return false;
    }
  }

  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::boundary_conditions<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>>(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>>&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>>&);
#endif
