// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "sort_triangles.h"
#include "barycenter.h"
#include "sort.h"
#include "sortrows.h"
#include "slice.h"
#include "round.h"
#include "colon.h"

#include <iostream>

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedMV,
  typename DerivedP,
  typename DerivedFF,
  typename DerivedI>
IGL_INLINE void igl::sort_triangles(
  const Eigen::PlainObjectBase<DerivedV> & V,
  const Eigen::PlainObjectBase<DerivedF> & F,
  const Eigen::PlainObjectBase<DerivedMV> & MV,
  const Eigen::PlainObjectBase<DerivedP> & P,
  Eigen::PlainObjectBase<DerivedFF> & FF,
  Eigen::PlainObjectBase<DerivedI> & I)
{
  using namespace Eigen;
  using namespace std;


  // Barycenter, centroid
  Eigen::Matrix<typename DerivedV::Scalar, DerivedF::RowsAtCompileTime,1> D,sD;
  Eigen::Matrix<typename DerivedV::Scalar, DerivedF::RowsAtCompileTime,4> BC,PBC;
  barycenter(V,F,BC);
  D = BC*(MV.transpose()*P.transpose().eval().col(2));
  sort(D,1,false,sD,I);

  //// Closest corner
  //Eigen::Matrix<typename DerivedV::Scalar, DerivedF::RowsAtCompileTime,1> D,sD;
  //D.setConstant(F.rows(),1,-1e26);
  //for(int c = 0;c<3;c++)
  //{
  //  Eigen::Matrix<typename DerivedV::Scalar, DerivedF::RowsAtCompileTime,4> C;
  //  Eigen::Matrix<typename DerivedV::Scalar, DerivedF::RowsAtCompileTime,1> DC;
  //  C.resize(F.rows(),4);
  //  for(int f = 0;f<F.rows();f++)
  //  {
  //    C(f,0) = V(F(f,c),0);
  //    C(f,1) = V(F(f,c),1);
  //    C(f,2) = V(F(f,c),2);
  //    C(f,3) = 1;
  //  }
  //  DC = C*(MV.transpose()*P.transpose().eval().col(2));
  //  D = (DC.array()>D.array()).select(DC,D).eval();
  //}
  //sort(D,1,false,sD,I);

  //// Closest corner with tie breaks
  //Eigen::Matrix<typename DerivedV::Scalar, DerivedF::RowsAtCompileTime,3> D,sD,ssD;
  //D.resize(F.rows(),3);
  //for(int c = 0;c<3;c++)
  //{
  //  Eigen::Matrix<typename DerivedV::Scalar, DerivedF::RowsAtCompileTime,4> C;
  //  C.resize(F.rows(),4);
  //  for(int f = 0;f<F.rows();f++)
  //  {
  //    C(f,0) = V(F(f,c),0);
  //    C(f,1) = V(F(f,c),1);
  //    C(f,2) = V(F(f,c),2);
  //    C(f,3) = 1;
  //  }
  //  D.col(c) = C*(MV.transpose()*P.transpose().eval().col(2));
  //}
  //VectorXi _;
  //sort(D,2,false,sD,_);
  //sortrows(sD,false,ssD,I);


  slice(F,I,1,FF);
}

//#include "EPS.h"
//#include <functional>
//#include <algorithm>
//
//static int tough_count = 0;
//template <typename Vec3>
//class Triangle
//{
//  public:
//    static inline bool z_comp(const Vec3 & A, const Vec3 & B)
//    {
//      return A(2) > B(2);
//    }
//   static typename Vec3::Scalar ZERO()
//   {
//     return igl::EPS<typename Vec3::Scalar>();
//     return 0;
//   }
//  public:
//    int id;
//    // Sorted projected coners: c[0] has smallest z value
//    Vec3 c[3];
//    Vec3 n;
//  public:
//    Triangle():id(-1) { };
//    Triangle(int id, const Vec3 c0, const Vec3 c1, const Vec3 c2):
//      id(id)
//    {
//      using namespace std;
//      c[0] = c0;
//      c[1] = c1;
//      c[2] = c2;
//      sort(c,c+3,Triangle<Vec3>::z_comp);
//      // normal pointed toward viewpoint
//      n = (c0-c1).cross(c2-c0);
//      if(n(2) < 0)
//      {
//        n *= -1.0;
//      }
//      // Avoid NaNs
//      typename Vec3::Scalar len = n.norm();
//      if(len == 0)
//      {
//        cout<<"avoid NaN"<<endl;
//        assert(false);
//        len = 1;
//      }
//      n /= len;
//    };
//
//    typename Vec3::Scalar project(const Vec3 & r) const
//    {
//      //return n.dot(r-c[2]);
//      int closest = -1;
//      typename Vec3::Scalar min_dist = 1e26;
//      for(int ci = 0;ci<3;ci++)
//      {
//        typename Vec3::Scalar dist = (c[ci]-r).norm();
//        if(dist < min_dist)
//        {
//          min_dist = dist;
//          closest = ci;
//        }
//      }
//      assert(closest>=0);
//      return n.dot(r-c[closest]);
//    }
//
//    // Z-values of this are < z-values of that
//    bool is_completely_behind(const Triangle & that) const
//    {
//      const typename Vec3::Scalar ac0 = that.c[0](2);
//      const typename Vec3::Scalar ac1 = that.c[1](2);
//      const typename Vec3::Scalar ac2 = that.c[2](2);
//      const typename Vec3::Scalar ic0 = this->c[0](2);
//      const typename Vec3::Scalar ic1 = this->c[1](2);
//      const typename Vec3::Scalar ic2 = this->c[2](2);
//      return
//        (ic0 <  ac2 && ic1 <= ac2 && ic2 <= ac2) ||
//        (ic0 <= ac2 && ic1 <  ac2 && ic2 <= ac2) ||
//        (ic0 <= ac2 && ic1 <= ac2 && ic2 <  ac2);
//    }
//
//    bool is_behind_plane(const Triangle &that) const
//    {
//      using namespace std;
//      const typename Vec3::Scalar apc0 = that.project(this->c[0]);
//      const typename Vec3::Scalar apc1 = that.project(this->c[1]);
//      const typename Vec3::Scalar apc2 = that.project(this->c[2]);
//      cout<<"    "<<
//        apc0<<", "<<
//        apc1<<", "<<
//        apc2<<", "<<endl;
//      return (apc0 <  ZERO() && apc1 < ZERO() && apc2 < ZERO());
//    }
//
//    bool is_in_front_of_plane(const Triangle &that) const
//    {
//      using namespace std;
//      const typename Vec3::Scalar apc0 = that.project(this->c[0]);
//      const typename Vec3::Scalar apc1 = that.project(this->c[1]);
//      const typename Vec3::Scalar apc2 = that.project(this->c[2]);
//      cout<<"    "<<
//        apc0<<", "<<
//        apc1<<", "<<
//        apc2<<", "<<endl;
//      return (apc0 >  ZERO() && apc1 > ZERO() && apc2 > ZERO());
//    }
//
//    bool is_coplanar(const Triangle &that) const
//    {
//      using namespace std;
//      const typename Vec3::Scalar apc0 = that.project(this->c[0]);
//      const typename Vec3::Scalar apc1 = that.project(this->c[1]);
//      const typename Vec3::Scalar apc2 = that.project(this->c[2]);
//      return (fabs(apc0)<=ZERO() && fabs(apc1)<=ZERO() && fabs(apc2)<=ZERO());
//    }
//
//    // http://stackoverflow.com/a/14561664/148668
//    // a1 is line1 start, a2 is line1 end, b1 is line2 start, b2 is line2 end
//    static bool seg_seg_intersect(const Vec3 &  a1, const Vec3 &  a2, const Vec3 &  b1, const Vec3 &  b2)
//    {
//      Vec3 b = a2-a1;
//      Vec3 d = b2-b1;
//      typename Vec3::Scalar bDotDPerp = b(0) * d(1) - b(1) * d(0);
//
//      // if b dot d == 0, it means the lines are parallel so have infinite intersection points
//      if (bDotDPerp == 0)
//        return false;
//
//      Vec3 c = b1-a1;
//      typename Vec3::Scalar t = (c(0) * d(1) - c(1) * d(0)) / bDotDPerp;
//      if (t < 0 || t > 1)
//        return false;
//
//      typename Vec3::Scalar u = (c(0) * b(1) - c(1) * b(0)) / bDotDPerp;
//      if (u < 0 || u > 1)
//        return false;
//
//      return true;
//    }
//    bool has_corner_inside(const Triangle & that) const
//    {
//      // http://www.blackpawn.com/texts/pointinpoly/
//      // Compute vectors        
//      Vec3 A = that.c[0];
//      Vec3 B = that.c[1];
//      Vec3 C = that.c[2];
//      A(2) = B(2) = C(2) = 0;
//      for(int ci = 0;ci<3;ci++)
//      {
//        Vec3 P = this->c[ci];
//        P(2) = 0;
//
//        Vec3 v0 = C - A;
//        Vec3 v1 = B - A;
//        Vec3 v2 = P - A;
//        
//        // Compute dot products
//        typename Vec3::Scalar dot00 = v0.dot(v0);
//        typename Vec3::Scalar dot01 = v0.dot(v1);
//        typename Vec3::Scalar dot02 = v0.dot(v2);
//        typename Vec3::Scalar dot11 = v1.dot(v1);
//        typename Vec3::Scalar dot12 = v1.dot(v2);
//        
//        // Compute barycentric coordinates
//        typename Vec3::Scalar invDenom = 1 / (dot00 * dot11 - dot01 * dot01);
//        typename Vec3::Scalar u = (dot11 * dot02 - dot01 * dot12) * invDenom;
//        typename Vec3::Scalar v = (dot00 * dot12 - dot01 * dot02) * invDenom;
//        
//        // Check if point is in triangle
//        if((u >= 0) && (v >= 0) && (u + v < 1))
//        {
//          return true;
//        }
//      }
//      return false;
//    }
//
//    bool overlaps(const Triangle &that) const
//    {
//      // Edges cross
//      for(int e = 0;e<3;e++)
//      {
//        for(int f = 0;f<3;f++)
//        {
//          if(seg_seg_intersect(
//            this->c[e],this->c[(e+1)%3],
//            that.c[e],that.c[(e+1)%3]))
//          {
//            return true;
//          }
//        }
//      }
//      // This could be entirely inside that
//      if(this->has_corner_inside(that))
//      {
//        return true;
//      }
//      // vice versa
//      if(that.has_corner_inside(*this))
//      {
//        return true;
//      }
//      return false;
//    }
//
//
//    bool operator< (const Triangle &that) const
//    {
//      // THIS < THAT if "depth" of THIS  < "depth" of THAT
//      //      "      if THIS should be draw before THAT
//      using namespace std;
//      bool ret = false;
//      // Self compare
//      if(that.id == this->id)
//      {
//        ret = false;
//      }
//      if(this->is_completely_behind(that))
//      {
//        cout<<" "<<this->id<<" completely behind "<<that.id<<endl;
//        ret = false;
//      }else if(that.is_completely_behind(*this))
//      {
//        cout<<" "<<that.id<<" completely behind "<<this->id<<endl;
//        ret = true;
//      }else
//      {
//        if(!this->overlaps(that))
//        {
//          assert(!that.overlaps(*this));
//          cout<<"  THIS does not overlap THAT"<<endl;
//          // No overlap use barycenter
//          return 
//            1./3.*(this->c[0](2) + this->c[1](2) + this->c[2](2)) >
//            1./3.*(that.c[0](2) + that.c[1](2) + that.c[2](2));
//        }else
//        {
//          if(this->is_coplanar(that) || that.is_coplanar(*this))
//          { 
//            cout<<"  coplanar"<<endl;
//            // co-planar: decide based on barycenter depth
//            ret = 
//              1./3.*(this->c[0](2) + this->c[1](2) + this->c[2](2)) >
//              1./3.*(that.c[0](2) + that.c[1](2) + that.c[2](2));
//          }else if(this->is_behind_plane(that))
//          {
//            cout<<"  THIS behind plane of THAT"<<endl;
//            ret = true;
//          }else if(that.is_behind_plane(*this))
//          { 
//            cout<<"  THAT behind of plane of THIS"<<endl;
//            ret = false;
//          // THAT is in front of plane of THIS
//          }else if(that.is_in_front_of_plane(*this))
//          { 
//            cout<<"  THAT in front of plane of THIS"<<endl;
//            ret = true;
//          // THIS is in front of plane of THAT
//          }else if(this->is_in_front_of_plane(that))
//          {
//            cout<<"  THIS in front plane of THAT"<<endl;
//            ret = false;
//          }else
//          {
//            cout<<"  compare bary"<<endl;
//            ret = 
//              1./3.*(this->c[0](2) + this->c[1](2) + this->c[2](2)) >
//              1./3.*(that.c[0](2) + that.c[1](2) + that.c[2](2));
//          }
//        }
//      }
//      if(ret)
//      {
//        // THIS < THAT so better not be THAT < THIS
//        cout<<this->id<<" < "<<that.id<<endl;
//        assert(!(that < *this));
//      }else
//      {
//        // THIS >= THAT so could be THAT < THIS or THAT == THIS
//      }
//      return ret;
//    }
//};
//#include <igl/matlab/MatlabWorkspace.h>
//
//template <
//  typename DerivedV,
//  typename DerivedF,
//  typename DerivedMV,
//  typename DerivedP,
//  typename DerivedFF,
//  typename DerivedI>
//IGL_INLINE void igl::sort_triangles_robust(
//  const Eigen::PlainObjectBase<DerivedV> & V,
//  const Eigen::PlainObjectBase<DerivedF> & F,
//  const Eigen::PlainObjectBase<DerivedMV> & MV,
//  const Eigen::PlainObjectBase<DerivedP> & P,
//  Eigen::PlainObjectBase<DerivedFF> & FF,
//  Eigen::PlainObjectBase<DerivedI> & I)
//{
//  assert(false && 
//    "THIS WILL NEVER WORK because depth sorting is not a numerical sort where"
//    "pairwise comparisons of triangles are transitive.  Rather it is a"
//    "topological sort on a dependency graph. Dependency encodes 'This triangle"
//    "must be drawn before that one'");
//  using namespace std;
//  using namespace Eigen;
//  typedef Matrix<typename DerivedV::Scalar,3,1> Vec3;
//  assert(V.cols() == 4);
//  Matrix<typename DerivedV::Scalar, DerivedV::RowsAtCompileTime,3> VMVP =
//    V*(MV.transpose()*P.transpose().eval().block(0,0,4,3));
//
//  MatrixXd projV(V.rows(),3);
//  for(int v = 0;v<V.rows();v++)
//  {
//    Vector3d vv;
//    vv(0) = V(v,0);
//    vv(1) = V(v,1);
//    vv(2) = V(v,2);
//    Vector3d p;
//    project(vv,p);
//    projV.row(v) = p;
//  }
//
//  vector<Triangle<Vec3> > vF(F.rows());
//  MatrixXd N(F.rows(),3);
//  MatrixXd C(F.rows()*3,3);
//  for(int f = 0;f<F.rows();f++)
//  {
//    vF[f] = 
//      //Triangle<Vec3>(f,VMVP.row(F(f,0)),VMVP.row(F(f,1)),VMVP.row(F(f,2)));
//      Triangle<Vec3>(f,projV.row(F(f,0)),projV.row(F(f,1)),projV.row(F(f,2)));
//    N.row(f) = vF[f].n;
//    for(int c = 0;c<3;c++)
//      for(int d = 0;d<3;d++)
//        C(f*3+c,d) = vF[f].c[c](d);
//  }
//  MatlabWorkspace mw;
//  mw.save_index(F,"F");
//  mw.save(V,"V");
//  mw.save(MV,"MV");
//  mw.save(P,"P");
//  Vector4i VP;
//  glGetIntegerv(GL_VIEWPORT, VP.data());
//  mw.save(projV,"projV");
//  mw.save(VP,"VP");
//  mw.save(VMVP,"VMVP");
//  mw.save(N,"N");
//  mw.save(C,"C");
//  mw.write("ao.mat");
//  sort(vF.begin(),vF.end());
//
//  // check
//  for(int f = 0;f<F.rows();f++)
//  {
//    for(int g = f+1;g<F.rows();g++)
//    {
//      assert(!(vF[g] < vF[f])); // should never happen
//    }
//  }
//  FF.resize(F.rows(),3);
//  I.resize(F.rows(),1);
//  for(int f = 0;f<F.rows();f++)
//  {
//    FF.row(f) = F.row(vF[f].id);
//    I(f) = vF[f].id;
//  }
//
//  mw.save_index(FF,"FF");
//  mw.save_index(I,"I");
//  mw.write("ao.mat");
//}

//template <
//  typename DerivedV,
//  typename DerivedF,
//  typename DerivedFF,
//  typename DerivedI>
//IGL_INLINE void igl::sort_triangles_robust(
//  const Eigen::PlainObjectBase<DerivedV> & V,
//  const Eigen::PlainObjectBase<DerivedF> & F,
//  Eigen::PlainObjectBase<DerivedFF> & FF,
//  Eigen::PlainObjectBase<DerivedI> & I)
//{
//  using namespace Eigen;
//  using namespace std;
//  // Put model, projection, and viewport matrices into double arrays
//  Matrix4d MV;
//  Matrix4d P;
//  glGetDoublev(GL_MODELVIEW_MATRIX,  MV.data());
//  glGetDoublev(GL_PROJECTION_MATRIX, P.data());
//  if(V.cols() == 3)
//  {
//    Matrix<typename DerivedV::Scalar, DerivedV::RowsAtCompileTime,4> hV;
//    hV.resize(V.rows(),4);
//    hV.block(0,0,V.rows(),V.cols()) = V;
//    hV.col(3).setConstant(1);
//    return sort_triangles_robust(hV,F,MV,P,FF,I);
//  }else
//  {
//    return sort_triangles_robust(V,F,MV,P,FF,I);
//  }
//}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::sort_triangles<Eigen::Matrix<double, -1, 4, 0, -1, 4>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 4, 4, 0, 4, 4>, Eigen::Matrix<double, 4, 4, 0, 4, 4>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 4, 0, -1, 4> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 4, 4, 0, 4, 4> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 4, 4, 0, 4, 4> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template void igl::sort_triangles<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, 4, 4, 0, 4, 4>, Eigen::Matrix<double, 4, 4, 0, 4, 4>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 4, 4, 0, 4, 4> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, 4, 4, 0, 4, 4> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
