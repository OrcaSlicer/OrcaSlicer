// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "comb_cross_field.h"

#include <vector>
#include <deque>
#include <Eigen/Geometry>
#include "per_face_normals.h"
#include "is_border_vertex.h"
#include "rotation_matrix_from_directions.h"

#include "triangle_triangle_adjacency.h"

namespace igl {
  template <typename DerivedV, typename DerivedF>
  class Comb
  {
  public:

    const Eigen::PlainObjectBase<DerivedV> &V;
    const Eigen::PlainObjectBase<DerivedF> &F;
    const Eigen::PlainObjectBase<DerivedV> &PD1;
    const Eigen::PlainObjectBase<DerivedV> &PD2;
    DerivedV N;

  private:
    // internal
    DerivedF TT;
    DerivedF TTi;


  private:


    static inline double Sign(double a){return (double)((a>0)?+1:-1);}


  private:

    // returns the 90 deg rotation of a (around n) most similar to target b
    /// a and b should be in the same plane orthogonal to N
    static inline Eigen::Matrix<typename DerivedV::Scalar, 3, 1> K_PI_new(const Eigen::Matrix<typename DerivedV::Scalar, 3, 1>& a,
                                                                   const Eigen::Matrix<typename DerivedV::Scalar, 3, 1>& b,
                                                                   const Eigen::Matrix<typename DerivedV::Scalar, 3, 1>& n)
    {
      Eigen::Matrix<typename DerivedV::Scalar, 3, 1> c = (a.cross(n)).normalized();
      typename DerivedV::Scalar scorea = a.dot(b);
      typename DerivedV::Scalar scorec = c.dot(b);
      if (fabs(scorea)>=fabs(scorec))
        return a*Sign(scorea);
      else
        return c*Sign(scorec);
    }



  public:
    inline Comb(const Eigen::PlainObjectBase<DerivedV> &_V,
         const Eigen::PlainObjectBase<DerivedF> &_F,
         const Eigen::PlainObjectBase<DerivedV> &_PD1,
         const Eigen::PlainObjectBase<DerivedV> &_PD2
         ):
    V(_V),
    F(_F),
    PD1(_PD1),
    PD2(_PD2)
    {
      igl::per_face_normals(V,F,N);
      igl::triangle_triangle_adjacency(F,TT,TTi);
    }
    inline void comb(Eigen::PlainObjectBase<DerivedV> &PD1out,
              Eigen::PlainObjectBase<DerivedV> &PD2out)
    {
//      PD1out = PD1;
//      PD2out = PD2;
      PD1out.setZero(F.rows(),3);PD1out<<PD1;
      PD2out.setZero(F.rows(),3);PD2out<<PD2;

      Eigen::VectorXi mark = Eigen::VectorXi::Constant(F.rows(),false);

      std::deque<int> d;

      d.push_back(0);
      mark(0) = true;

      while (!d.empty())
      {
        int f0 = d.at(0);
        d.pop_front();
        for (int k=0; k<3; k++)
        {
          int f1 = TT(f0,k);
          if (f1==-1) continue;
          if (mark(f1)) continue;

          Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir0    = PD1out.row(f0);
          Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir1    = PD1out.row(f1);
          Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n0    = N.row(f0);
          Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n1    = N.row(f1);


          Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir0Rot = igl::rotation_matrix_from_directions(n0, n1)*dir0;
          dir0Rot.normalize();
          Eigen::Matrix<typename DerivedV::Scalar, 3, 1> targD   = K_PI_new(dir1,dir0Rot,n1);

          PD1out.row(f1)  = targD;
          PD2out.row(f1)  = n1.cross(targD).normalized();

          mark(f1) = true;
          d.push_back(f1);

        }
      }

      // everything should be marked
      for (int i=0; i<F.rows(); i++)
      {
        assert(mark(i));
      }
    }



  };
}
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::comb_cross_field(const Eigen::PlainObjectBase<DerivedV> &V,
                                      const Eigen::PlainObjectBase<DerivedF> &F,
                                      const Eigen::PlainObjectBase<DerivedV> &PD1,
                                      const Eigen::PlainObjectBase<DerivedV> &PD2,
                                      Eigen::PlainObjectBase<DerivedV> &PD1out,
                                      Eigen::PlainObjectBase<DerivedV> &PD2out)
{
  igl::Comb<DerivedV, DerivedF> cmb(V, F, PD1, PD2);
  cmb.comb(PD1out, PD2out);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::comb_cross_field<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&);
template void igl::comb_cross_field<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&);
#endif
