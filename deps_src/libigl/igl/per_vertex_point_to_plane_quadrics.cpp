// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "per_vertex_point_to_plane_quadrics.h"
#include "quadric_binary_plus_operator.h"
#include <Eigen/QR>
#include <cassert>
#include <cmath>


IGL_INLINE void igl::per_vertex_point_to_plane_quadrics(
  const Eigen::MatrixXd & V,
  const Eigen::MatrixXi & F,
  const Eigen::MatrixXi & EMAP,
  const Eigen::MatrixXi & EF,
  const Eigen::MatrixXi & EI,
  std::vector<
    std::tuple<Eigen::MatrixXd,Eigen::RowVectorXd,double> > & quadrics)
{
  using namespace std;
  typedef std::tuple<Eigen::MatrixXd,Eigen::RowVectorXd,double> Quadric;
  const int dim = V.cols();
  //// Quadrics per face
  //std::vector<Quadric> face_quadrics(F.rows());
  // Initialize each vertex quadric to zeros
  quadrics.resize(
    V.rows(),
    // gcc <=4.8 can't handle initializer lists correctly
    Quadric{Eigen::MatrixXd::Zero(dim,dim),Eigen::RowVectorXd::Zero(dim),0});
  Eigen::MatrixXd I = Eigen::MatrixXd::Identity(dim,dim);
  // Rather initial with zeros, initial with a small amount of energy pull
  // toward original vertex position
  const double w = 1e-10;
  for(int v = 0;v<V.rows();v++)
  {
    std::get<0>(quadrics[v]) = w*I;
    Eigen::RowVectorXd Vv = V.row(v);
    std::get<1>(quadrics[v]) = w*-Vv;
    std::get<2>(quadrics[v]) = w*Vv.dot(Vv);
  }
  // Generic nD qslim from "Simplifying Surfaces with Color and Texture
  // using Quadric Error Metric" (follow up to original QSlim)
  for(int f = 0;f<F.rows();f++)
  {
    int infinite_corner = -1;
    for(int c = 0;c<3;c++)
    {
      if(
         std::isinf(V(F(f,c),0)) || 
         std::isinf(V(F(f,c),1)) || 
         std::isinf(V(F(f,c),2)))
      {
        assert(infinite_corner == -1 && "Should only be one infinite corner");
        infinite_corner = c;
      }
    }
    // Inputs:
    //   p  1 by n row point on the subspace 
    //   S  m by n matrix where rows coorespond to orthonormal spanning
    //     vectors of the subspace to which we're measuring distance (usually
    //     a plane, m=2)
    //   weight  scalar weight
    // Returns quadric triple {A,b,c} so that A-2*b+c measures the quadric
    const auto subspace_quadric = [&I](
      const Eigen::RowVectorXd & p,
      const Eigen::MatrixXd & S,
      const double  weight)->Quadric
    {
      // Dimension of subspace
      const int m = S.rows();
      // Weight face's quadric (v'*A*v + 2*b'*v + c) by area
      // e1 and e2 should be perpendicular
      Eigen::MatrixXd A = I;
      Eigen::RowVectorXd b = -p;
      double c = p.dot(p);
      for(int i = 0;i<m;i++)
      {
        Eigen::RowVectorXd ei = S.row(i);
        for(int j = 0;j<i;j++) assert(std::abs(S.row(j).dot(ei)) < 1e-10);
        A += -ei.transpose()*ei;
        b += p.dot(ei)*ei;
        c += -pow(p.dot(ei),2);
      }
      // gcc <=4.8 can't handle initializer lists correctly: needs explicit
      // cast
      return Quadric{ weight*A, weight*b, weight*c };
    };
    if(infinite_corner == -1)
    {
      // Finite (non-boundary) face
      Eigen::RowVectorXd p = V.row(F(f,0));
      Eigen::RowVectorXd q = V.row(F(f,1));
      Eigen::RowVectorXd r = V.row(F(f,2));
      Eigen::RowVectorXd pq = q-p;
      Eigen::RowVectorXd pr = r-p;
      // Gram Determinant = squared area of parallelogram 
      double area = sqrt(pq.squaredNorm()*pr.squaredNorm()-pow(pr.dot(pq),2));
      Eigen::RowVectorXd e1 = pq.normalized();
      Eigen::RowVectorXd e2 = (pr-e1.dot(pr)*e1).normalized();
      Eigen::MatrixXd S(2,V.cols());
      S<<e1,e2;
      Quadric face_quadric = subspace_quadric(p,S,area);
      // Throw at each corner
      for(int c = 0;c<3;c++)
      {
        quadrics[F(f,c)] = quadrics[F(f,c)] + face_quadric;
      }
    }else
    {
      // cth corner is infinite --> edge opposite cth corner is boundary
      // Boundary edge vector
      const Eigen::RowVectorXd p = V.row(F(f,(infinite_corner+1)%3));
      Eigen::RowVectorXd ev = V.row(F(f,(infinite_corner+2)%3)) - p;
      const double length = ev.norm();
      ev /= length;
      // Face neighbor across boundary edge
      int e = EMAP(f+F.rows()*infinite_corner);
      int opp = EF(e,0) == f ? 1 : 0;
      int n =  EF(e,opp);
      int nc = EI(e,opp);
      assert(
        ((F(f,(infinite_corner+1)%3) == F(n,(nc+1)%3) && 
          F(f,(infinite_corner+2)%3) == F(n,(nc+2)%3)) || 
          (F(f,(infinite_corner+1)%3) == F(n,(nc+2)%3) 
          && F(f,(infinite_corner+2)%3) == F(n,(nc+1)%3))) && 
        "Edge flaps not agreeing on shared edge");
      // Edge vector on opposite face
      const Eigen::RowVectorXd eu = V.row(F(n,nc)) - p;
      assert(!std::isinf(eu(0)));
      // Matrix with vectors spanning plane as columns
      Eigen::MatrixXd A(ev.size(),2);
      A<<ev.transpose(),eu.transpose();
      // Use QR decomposition to find basis for orthogonal space
      Eigen::HouseholderQR<Eigen::MatrixXd> qr(A);
      const Eigen::MatrixXd Q = qr.householderQ();
      const Eigen::MatrixXd N = 
        Q.topRightCorner(ev.size(),ev.size()-2).transpose();
      assert(N.cols() == ev.size());
      assert(N.rows() == ev.size()-2);
      Eigen::MatrixXd S(N.rows()+1,ev.size());
      S<<ev,N;
      Quadric boundary_edge_quadric = subspace_quadric(p,S,length);
      for(int c = 0;c<3;c++)
      {
        if(c != infinite_corner)
        {
          quadrics[F(f,c)] = quadrics[F(f,c)] + boundary_edge_quadric;
        }
      }
    }
  }
}

