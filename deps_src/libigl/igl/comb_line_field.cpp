// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Nico Pietroni <nico.pietroni@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "comb_line_field.h"

#include <vector>
#include <deque>
#include "per_face_normals.h"
#include "is_border_vertex.h"
#include "rotation_matrix_from_directions.h"

#include "triangle_triangle_adjacency.h"
#include "PlainMatrix.h"

namespace igl {
template <typename DerivedV, typename DerivedF>
class CombLine
{
public:

    const Eigen::MatrixBase<DerivedV> &V;
    const Eigen::MatrixBase<DerivedF> &F;
    const Eigen::MatrixBase<DerivedV> &PD1;
    PlainMatrix<DerivedV> N;

private:
    // internal
    PlainMatrix<DerivedF> TT;
    PlainMatrix<DerivedF> TTi;


private:


    static inline double Sign(double a){return (double)((a>0)?+1:-1);}


private:

    // returns the 180 deg rotation of a (around n) most similar to target b
    // a and b should be in the same plane orthogonal to N
    static inline Eigen::Matrix<typename DerivedV::Scalar, 3, 1> K_PI_line(const Eigen::Matrix<typename DerivedV::Scalar, 3, 1>& a,
                                                                           const Eigen::Matrix<typename DerivedV::Scalar, 3, 1>& b)
    {
        typename DerivedV::Scalar scorea = a.dot(b);
        if (scorea<0)
            return -a;
        else
            return a;
    }



public:

    inline CombLine(const Eigen::MatrixBase<DerivedV> &_V,
                    const Eigen::MatrixBase<DerivedF> &_F,
                    const Eigen::MatrixBase<DerivedV> &_PD1):
        V(_V),
        F(_F),
        PD1(_PD1)
    {
        igl::per_face_normals(V,F,N);
        igl::triangle_triangle_adjacency(F,TT,TTi);
    }

    inline void comb(Eigen::PlainObjectBase<DerivedV> &PD1out)
    {
        PD1out.setZero(F.rows(),3);PD1out<<PD1;

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

                Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir0  = PD1out.row(f0);
                Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir1  = PD1out.row(f1);
                Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n0    = N.row(f0);
                Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n1    = N.row(f1);

                Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir0Rot = igl::rotation_matrix_from_directions(n0, n1)*dir0;
                dir0Rot.normalize();
                Eigen::Matrix<typename DerivedV::Scalar, 3, 1> targD   = K_PI_line(dir1,dir0Rot);

                PD1out.row(f1)  = targD;
                //PD2out.row(f1)  = n1.cross(targD).normalized();

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
IGL_INLINE void igl::comb_line_field(const Eigen::MatrixBase<DerivedV> &V,
                                     const Eigen::MatrixBase<DerivedF> &F,
                                     const Eigen::MatrixBase<DerivedV> &PD1,
                                     Eigen::PlainObjectBase<DerivedV> &PD1out)
{
    igl::CombLine<DerivedV, DerivedF> cmb(V, F, PD1);
    cmb.comb(PD1out);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
