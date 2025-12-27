// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2015 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "frame_field.h"

#include <igl/triangle_triangle_adjacency.h>
#include <igl/edge_topology.h>
#include <igl/per_face_normals.h>
#include <igl/copyleft/comiso/nrosy.h>
#include <iostream>

namespace igl
{
namespace copyleft
{
namespace comiso
{

class FrameInterpolator
{
public:
  // Init
  IGL_INLINE FrameInterpolator(const Eigen::MatrixXd& _V, const Eigen::MatrixXi& _F);
  IGL_INLINE ~FrameInterpolator();

  // Reset constraints (at least one constraint must be present or solve will fail)
  IGL_INLINE void resetConstraints();

  IGL_INLINE void setConstraint(const int fid, const Eigen::VectorXd& v);

  IGL_INLINE void interpolateSymmetric();

  // Generate the frame field
  IGL_INLINE void solve();

  // Convert the frame field in the canonical representation
  IGL_INLINE void frame2canonical(const Eigen::MatrixXd& TP, const Eigen::RowVectorXd& v, double& theta, Eigen::VectorXd& S);

  // Convert the canonical representation in a frame field
  IGL_INLINE void canonical2frame(const Eigen::MatrixXd& TP, const double theta, const Eigen::VectorXd& S, Eigen::RowVectorXd& v);

  IGL_INLINE Eigen::MatrixXd getFieldPerFace();

  IGL_INLINE void PolarDecomposition(Eigen::MatrixXd V, Eigen::MatrixXd& U, Eigen::MatrixXd& P);

  // Symmetric
  Eigen::MatrixXd S;
  std::vector<bool> S_c;

  // -------------------------------------------------

  // Face Topology
  Eigen::MatrixXi TT, TTi;

  // Two faces are consistent if their representative vector are taken modulo PI
  std::vector<bool> edge_consistency;
  Eigen::MatrixXi   edge_consistency_TT;

private:
  IGL_INLINE double mod2pi(double d);
  IGL_INLINE double modpi2(double d);
  IGL_INLINE double modpi(double d);

  // Convert a direction on the tangent space into an angle
  IGL_INLINE double vector2theta(const Eigen::MatrixXd& TP, const Eigen::RowVectorXd& v);

  // Convert an angle in a vector in the tangent space
  IGL_INLINE Eigen::RowVectorXd theta2vector(const Eigen::MatrixXd& TP, const double theta);

  // Interpolate the cross field (theta)
  IGL_INLINE void interpolateCross();

  // Compute difference between reference frames
  IGL_INLINE void computek();

  // Compute edge consistency
  IGL_INLINE void compute_edge_consistency();

  // Cross field direction
  Eigen::VectorXd thetas;
  std::vector<bool> thetas_c;

  // Edge Topology
  Eigen::MatrixXi EV, FE, EF;
  std::vector<bool> isBorderEdge;

  // Angle between two reference frames
  // R(k) * t0 = t1
  Eigen::VectorXd k;

  // Mesh
  Eigen::MatrixXd V;
  Eigen::MatrixXi F;

  // Normals per face
  Eigen::MatrixXd N;

  // Reference frame per triangle
  std::vector<Eigen::MatrixXd> TPs;

};

FrameInterpolator::FrameInterpolator(const Eigen::MatrixXd& _V, const Eigen::MatrixXi& _F)
{
  using namespace std;
  using namespace Eigen;

  V = _V;
  F = _F;

  assert(V.rows() > 0);
  assert(F.rows() > 0);


  // Generate topological relations
  igl::triangle_triangle_adjacency(F,TT,TTi);
  igl::edge_topology(V,F, EV, FE, EF);

  // Flag border edges
  isBorderEdge.resize(EV.rows());
  for(unsigned i=0; i<EV.rows(); ++i)
    isBorderEdge[i] = (EF(i,0) == -1) || ((EF(i,1) == -1));

  // Generate normals per face
  igl::per_face_normals(V, F, N);

  // Generate reference frames
  for(unsigned fid=0; fid<F.rows(); ++fid)
  {
    // First edge
    Vector3d e1 = V.row(F(fid,1)) - V.row(F(fid,0));
    e1.normalize();
    Vector3d e2 = N.row(fid);
    e2 = e2.cross(e1);
    e2.normalize();

    MatrixXd TP(2,3);
    TP << e1.transpose(), e2.transpose();
    TPs.push_back(TP);
  }

  // Reset the constraints
  resetConstraints();

  // Compute k, differences between reference frames
  computek();

  // Alloc internal variables
  thetas            = VectorXd::Zero(F.rows());
  S = MatrixXd::Zero(F.rows(),3);

  compute_edge_consistency();
}

FrameInterpolator::~FrameInterpolator()
{

}

double FrameInterpolator::mod2pi(double d)
{
  while(d<0)
    d = d + (2.0*igl::PI);

  return fmod(d, (2.0*igl::PI));
}

double FrameInterpolator::modpi2(double d)
{
  while(d<0)
    d = d + (igl::PI/2.0);

  return fmod(d, (igl::PI/2.0));
}

double FrameInterpolator::modpi(double d)
{
  while(d<0)
    d = d + (igl::PI);

  return fmod(d, (igl::PI));
}


double FrameInterpolator::vector2theta(const Eigen::MatrixXd& TP, const Eigen::RowVectorXd& v)
{
  // Project onto the tangent plane
  Eigen::Vector2d vp = TP * v.transpose();

  // Convert to angle
  double theta = atan2(vp(1),vp(0));
  return theta;
}

Eigen::RowVectorXd FrameInterpolator::theta2vector(const Eigen::MatrixXd& TP, const double theta)
{
  Eigen::Vector2d vp(cos(theta),sin(theta));
  return vp.transpose() * TP;
}

void FrameInterpolator::interpolateCross()
{
  using namespace std;
  using namespace Eigen;

  //olga: was
  // NRosyField nrosy(V,F);
  // for (unsigned i=0; i<F.rows(); ++i)
    // if(thetas_c[i])
      // nrosy.setConstraintHard(i,theta2vector(TPs[i],thetas(i)));
  // nrosy.solve(4);
  // MatrixXd R = nrosy.getFieldPerFace();

  //olga: is
  Eigen::MatrixXd R;
  Eigen::VectorXd S;
  Eigen::VectorXi b; b.resize(F.rows(),1);
  Eigen::MatrixXd bc; bc.resize(F.rows(),3);
  int num = 0;
  for (unsigned i=0; i<F.rows(); ++i)
    if(thetas_c[i])
      {
        b[num] = i;
        bc.row(num) = theta2vector(TPs[i],thetas(i));
        num++;
      }
  b.conservativeResize(num,Eigen::NoChange);
  bc.conservativeResize(num,Eigen::NoChange);

  igl::copyleft::comiso::nrosy(V, F, b, bc, 4, R, S);
  //olga:end
  assert(R.rows() == F.rows());

  for (unsigned i=0; i<F.rows(); ++i)
    thetas(i) = vector2theta(TPs[i],R.row(i));
}

void FrameInterpolator::resetConstraints()
{
  thetas_c.resize(F.rows());
  S_c.resize(F.rows());

  for(unsigned i=0; i<F.rows(); ++i)
  {
    thetas_c[i]  = false;
    S_c[i] = false;
  }

}

void FrameInterpolator::compute_edge_consistency()
{
  using namespace std;
  using namespace Eigen;

  // Compute per-edge consistency
  edge_consistency.resize(EF.rows());
  edge_consistency_TT = MatrixXi::Constant(TT.rows(),3,-1);

  // For every non-border edge
  for (unsigned eid=0; eid<EF.rows(); ++eid)
  {
    if (!isBorderEdge[eid])
    {
      int fid0 = EF(eid,0);
      int fid1 = EF(eid,1);

      double theta0 = thetas(fid0);
      double theta1 = thetas(fid1);

      theta0 = theta0 + k(eid);

      double r = modpi(theta0-theta1);

      edge_consistency[eid] = r < igl::PI/4.0 || r > 3*(igl::PI/4.0);

      // Copy it into edge_consistency_TT
      int i1 = -1;
      int i2 = -1;
      for (unsigned i=0; i<3; ++i)
      {
        if (TT(fid0,i) == fid1)
          i1 = i;
        if (TT(fid1,i) == fid0)
          i2 = i;
      }
      assert(i1 != -1);
      assert(i2 != -1);

      edge_consistency_TT(fid0,i1) = edge_consistency[eid];
      edge_consistency_TT(fid1,i2) = edge_consistency[eid];
    }
  }
}

void FrameInterpolator::computek()
{
  using namespace std;
  using namespace Eigen;

  k.resize(EF.rows());

  // For every non-border edge
  for (unsigned eid=0; eid<EF.rows(); ++eid)
  {
    if (!isBorderEdge[eid])
    {
      int fid0 = EF(eid,0);
      int fid1 = EF(eid,1);

      Vector3d N0 = N.row(fid0);
      //Vector3d N1 = N.row(fid1);

      // find common edge on triangle 0 and 1
      int fid0_vc = -1;
      int fid1_vc = -1;
      for (unsigned i=0;i<3;++i)
      {
        if (EV(eid,0) == F(fid0,i))
          fid0_vc = i;
        if (EV(eid,1) == F(fid1,i))
          fid1_vc = i;
      }
      assert(fid0_vc != -1);
      assert(fid1_vc != -1);

      Vector3d common_edge = V.row(F(fid0,(fid0_vc+1)%3)) - V.row(F(fid0,fid0_vc));
      common_edge.normalize();

      // Map the two triangles in a new space where the common edge is the x axis and the N0 the z axis
      MatrixXd P(3,3);
      VectorXd o = V.row(F(fid0,fid0_vc));
      VectorXd tmp = -N0.cross(common_edge);
      P << common_edge, tmp, N0;
      P.transposeInPlace();


      MatrixXd V0(3,3);
      V0.row(0) = V.row(F(fid0,0)).transpose() -o;
      V0.row(1) = V.row(F(fid0,1)).transpose() -o;
      V0.row(2) = V.row(F(fid0,2)).transpose() -o;

      V0 = (P*V0.transpose()).transpose();

      assert(V0(0,2) < 10e-10);
      assert(V0(1,2) < 10e-10);
      assert(V0(2,2) < 10e-10);

      MatrixXd V1(3,3);
      V1.row(0) = V.row(F(fid1,0)).transpose() -o;
      V1.row(1) = V.row(F(fid1,1)).transpose() -o;
      V1.row(2) = V.row(F(fid1,2)).transpose() -o;
      V1 = (P*V1.transpose()).transpose();

      assert(V1(fid1_vc,2) < 10e-10);
      assert(V1((fid1_vc+1)%3,2) < 10e-10);

      // compute rotation R such that R * N1 = N0
      // i.e. map both triangles to the same plane
      double alpha = -atan2(V1((fid1_vc+2)%3,2),V1((fid1_vc+2)%3,1));

      MatrixXd R(3,3);
      R << 1,          0,            0,
           0, cos(alpha), -sin(alpha) ,
           0, sin(alpha),  cos(alpha);
      V1 = (R*V1.transpose()).transpose();

      assert(V1(0,2) < 10e-10);
      assert(V1(1,2) < 10e-10);
      assert(V1(2,2) < 10e-10);

      // measure the angle between the reference frames
      // k_ij is the angle between the triangle on the left and the one on the right
      VectorXd ref0 = V0.row(1) - V0.row(0);
      VectorXd ref1 = V1.row(1) - V1.row(0);

      ref0.normalize();
      ref1.normalize();

      double ktemp = atan2(ref1(1),ref1(0)) - atan2(ref0(1),ref0(0));

      // just to be sure, rotate ref0 using angle ktemp...
      MatrixXd R2(2,2);
      R2 << cos(ktemp), -sin(ktemp), sin(ktemp), cos(ktemp);

      tmp = R2*ref0.head<2>();

      assert(tmp(0) - ref1(0) < (0.000001));
      assert(tmp(1) - ref1(1) < (0.000001));

      k[eid] = ktemp;
    }
  }

}


  void FrameInterpolator::frame2canonical(const Eigen::MatrixXd& TP, const Eigen::RowVectorXd& v, double& theta, Eigen::VectorXd& S_v)
{
  using namespace std;
  using namespace Eigen;

  RowVectorXd v0 = v.segment<3>(0);
  RowVectorXd v1 = v.segment<3>(3);

  // Project onto the tangent plane
  Vector2d vp0 = TP * v0.transpose();
  Vector2d vp1 = TP * v1.transpose();

  // Assemble matrix
  MatrixXd M(2,2);
  M << vp0, vp1;

  if (M.determinant() < 0)
    M.col(1) = -M.col(1);

  assert(M.determinant() > 0);

  // cerr << "M: " << M << endl;

  MatrixXd R,S;
  PolarDecomposition(M,R,S);

  // Finally, express the cross field as an angle
  theta = atan2(R(1,0),R(0,0));

  MatrixXd R2(2,2);
  R2 << cos(theta), -sin(theta), sin(theta), cos(theta);

  assert((R2-R).norm() < 10e-8);

  // Convert into rotation invariant form
  S = R * S * R.inverse();

  // Copy in vector form
  S_v = VectorXd(3);
  S_v << S(0,0), S(0,1), S(1,1);
}

  void FrameInterpolator::canonical2frame(const Eigen::MatrixXd& TP, const double theta, const Eigen::VectorXd& S_v, Eigen::RowVectorXd& v)
{
  using namespace std;
  using namespace Eigen;

  assert(S_v.size() == 3);

  MatrixXd S_temp(2,2);
  S_temp << S_v(0), S_v(1), S_v(1), S_v(2);

  // Convert angle in vector in the tangent plane
  // Vector2d vp(cos(theta),sin(theta));

  // First reconstruct R
  MatrixXd R(2,2);

  R << cos(theta), -sin(theta), sin(theta), cos(theta);

  // Rotation invariant reconstruction
  MatrixXd M = S_temp * R;

  Vector2d vp0(M(0,0),M(1,0));
  Vector2d vp1(M(0,1),M(1,1));

  // Unproject the vectors
  RowVectorXd v0 = vp0.transpose() * TP;
  RowVectorXd v1 = vp1.transpose() * TP;

  v.resize(6);
  v << v0, v1;
}

void FrameInterpolator::solve()
{
  interpolateCross();
  interpolateSymmetric();
}

void FrameInterpolator::interpolateSymmetric()
{
  using namespace std;
  using namespace Eigen;

  // Generate uniform Laplacian matrix
  typedef Eigen::Triplet<double> triplet;
  std::vector<triplet> triplets;

  // Variables are stacked as x1,y1,z1,x2,y2,z2
  triplets.reserve(3*4*F.rows());

  MatrixXd b = MatrixXd::Zero(3*F.rows(),1);

  // Build L and b
  for (unsigned eid=0; eid<EF.rows(); ++eid)
  {
    if (!isBorderEdge[eid])
    {
      for (int z=0;z<2;++z)
      {
        // W = [w_a, w_b
        //      w_b, w_c]
        //

        // It is not symmetric
        int i    = EF(eid,z==0?0:1);
        int j    = EF(eid,z==0?1:0);

        int w_a_0 = (i*3)+0;
        int w_b_0 = (i*3)+1;
        int w_c_0 = (i*3)+2;

        int w_a_1 = (j*3)+0;
        int w_b_1 = (j*3)+1;
        int w_c_1 = (j*3)+2;

        // Rotation to change frame
        double r_a =  cos(z==1?k(eid):-k(eid));
        double r_b = -sin(z==1?k(eid):-k(eid));
        double r_c =  sin(z==1?k(eid):-k(eid));
        double r_d =  cos(z==1?k(eid):-k(eid));

        // First term
        // w_a_0 = r_a^2 w_a_1 + 2 r_a r_b w_b_1 + r_b^2 w_c_1 = 0
        triplets.push_back(triplet(w_a_0,w_a_0,                -1 ));
        triplets.push_back(triplet(w_a_0,w_a_1,           r_a*r_a ));
        triplets.push_back(triplet(w_a_0,w_b_1,       2 * r_a*r_b ));
        triplets.push_back(triplet(w_a_0,w_c_1,           r_b*r_b ));

        // Second term
        // w_b_0 = r_a r_c w_a + (r_b r_c + r_a r_d) w_b + r_b r_d w_c
        triplets.push_back(triplet(w_b_0,w_b_0,                -1 ));
        triplets.push_back(triplet(w_b_0,w_a_1,           r_a*r_c ));
        triplets.push_back(triplet(w_b_0,w_b_1, r_b*r_c + r_a*r_d ));
        triplets.push_back(triplet(w_b_0,w_c_1,           r_b*r_d ));

        // Third term
        // w_c_0 = r_c^2 w_a + 2 r_c r_d w_b +  r_d^2 w_c
        triplets.push_back(triplet(w_c_0,w_c_0,                -1 ));
        triplets.push_back(triplet(w_c_0,w_a_1,           r_c*r_c ));
        triplets.push_back(triplet(w_c_0,w_b_1,       2 * r_c*r_d ));
        triplets.push_back(triplet(w_c_0,w_c_1,           r_d*r_d ));
      }
    }
  }

  SparseMatrix<double> L(3*F.rows(),3*F.rows());
  L.setFromTriplets(triplets.begin(), triplets.end());

  triplets.clear();

  // Add soft constraints
  double w = 100000;
  for (unsigned fid=0; fid < F.rows(); ++fid)
  {
    if (S_c[fid])
    {
      for (unsigned i=0;i<3;++i)
      {
        triplets.push_back(triplet(3*fid + i,3*fid + i,w));
        b(3*fid + i) += w*S(fid,i);
      }
    }
  }

  SparseMatrix<double> soft(3*F.rows(),3*F.rows());
  soft.setFromTriplets(triplets.begin(), triplets.end());

  SparseMatrix<double> M;

  M = L + soft;

  // Solve Lx = b;

  SparseLU<SparseMatrix<double> > solver;

  solver.compute(M);

  if(solver.info()!=Success)
  {
    std::cerr << "LU failed - frame_interpolator.cpp" << std::endl;
    assert(0);
  }

  MatrixXd x;
  x = solver.solve(b);

  if(solver.info()!=Success)
  {
    std::cerr << "Linear solve failed - frame_interpolator.cpp" << std::endl;
    assert(0);
  }

  S = MatrixXd::Zero(F.rows(),3);

  // Copy back the result
  for (unsigned i=0;i<F.rows();++i)
    S.row(i) << x(i*3+0), x(i*3+1), x(i*3+2);

}

void FrameInterpolator::setConstraint(const int fid, const Eigen::VectorXd& v)
{
  using namespace std;
  using namespace Eigen;

  double   t_;
  VectorXd S_;

  frame2canonical(TPs[fid],v,t_,S_);

  Eigen::RowVectorXd v2;
  canonical2frame(TPs[fid], t_, S_, v2);

  thetas(fid)   = t_;
  thetas_c[fid] = true;

  S.row(fid) = S_;
  S_c[fid]   = true;

}

Eigen::MatrixXd FrameInterpolator::getFieldPerFace()
{
  using namespace std;
  using namespace Eigen;

  MatrixXd R(F.rows(),6);
  for (unsigned i=0; i<F.rows(); ++i)
  {
    RowVectorXd v;
    canonical2frame(TPs[i],thetas(i),S.row(i),v);
    R.row(i) = v;
  }
  return R;
}

  void FrameInterpolator::PolarDecomposition(Eigen::MatrixXd V, Eigen::MatrixXd& U, Eigen::MatrixXd& P)
{
  using namespace std;
  using namespace Eigen;

  // Polar Decomposition
  JacobiSVD<MatrixXd> svd(V,Eigen::ComputeFullU | Eigen::ComputeFullV);

  U = svd.matrixU() * svd.matrixV().transpose();
  P = svd.matrixV() * svd.singularValues().asDiagonal() * svd.matrixV().transpose();
}

}
}
}

IGL_INLINE void igl::copyleft::comiso::frame_field(
                                 const Eigen::MatrixXd& V,
                                 const Eigen::MatrixXi& F,
                                 const Eigen::VectorXi& b,
                                 const Eigen::MatrixXd& bc1,
                                 const Eigen::MatrixXd& bc2,
                                 Eigen::MatrixXd& FF1,
                                 Eigen::MatrixXd& FF2
                                 )

{
  using namespace std;
  using namespace Eigen;

  assert(b.size() > 0);

  // Init Solver
  FrameInterpolator field(V,F);

  for (unsigned i=0; i<b.size(); ++i)
  {
    VectorXd t(6); t << bc1.row(i).transpose(), bc2.row(i).transpose();
    field.setConstraint(b(i), t);
  }

  // Solve
  field.solve();

  // Copy back
  MatrixXd R = field.getFieldPerFace();
  FF1 = R.block(0, 0, R.rows(), 3);
  FF2 = R.block(0, 3, R.rows(), 3);
}
