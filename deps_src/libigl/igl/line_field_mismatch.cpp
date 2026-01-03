// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Nico Pietroni <nico.pietroni@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "line_field_mismatch.h"

#include <vector>
#include <deque>
#include "comb_line_field.h"
#include "rotate_vectors.h"
#include "comb_cross_field.h"
#include "comb_line_field.h"
#include "per_face_normals.h"
#include "is_border_vertex.h"
#include "vertex_triangle_adjacency.h"
#include "triangle_triangle_adjacency.h"
#include "rotation_matrix_from_directions.h"
#include "local_basis.h"
#include "PI.h"
#include "PlainMatrix.h"

namespace igl {
template <typename DerivedV, typename DerivedF, typename DerivedO>
class MismatchCalculatorLine
{
public:

    const Eigen::MatrixBase<DerivedV> &V;
    const Eigen::MatrixBase<DerivedF> &F;
    const Eigen::MatrixBase<DerivedV> &PD1;
    const Eigen::MatrixBase<DerivedV> &PD2;
    PlainMatrix<DerivedV> N;

private:
    // internal
    std::vector<bool> V_border; // bool
    std::vector<std::vector<int> > VF;
    std::vector<std::vector<int> > VFi;
    PlainMatrix<DerivedF> TT;
    PlainMatrix<DerivedF> TTi;


private:

    //compute the mismatch between 2 faces
    inline int mismatchByLine(const int f0,
                              const int f1)
    {
        Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir0 = PD1.row(f0);
        Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir1 = PD1.row(f1);
        Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n0 = N.row(f0);
        Eigen::Matrix<typename DerivedV::Scalar, 3, 1> n1 = N.row(f1);

        Eigen::Matrix<typename DerivedV::Scalar, 3, 1> dir1Rot = igl::rotation_matrix_from_directions(n1,n0)*dir1;
        dir1Rot.normalize();

        // TODO: this should be equivalent to the other code below, to check!
        // Compute the angle between the two vectors
        //    double a0 = atan2(dir0.dot(B2.row(f0)),dir0.dot(B1.row(f0)));
        //    double a1 = atan2(dir1Rot.dot(B2.row(f0)),dir1Rot.dot(B1.row(f0)));
        //
        //    double angle_diff = a1-a0;   //VectToAngle(f0,dir1Rot);

        double angle_diff = atan2(dir1Rot.dot(PD2.row(f0)),dir1Rot.dot(PD1.row(f0)));

        double step=igl::PI;
        int i=(int)std::floor((angle_diff/step)+0.5);
        assert((i>=-2)&&(i<=2));
        int k=0;
        if (i>=0)
            k=i%2;
        else
            k=(2+i)%2;

        assert((k==0)||(k==1));
        return (k*2);
    }

public:

    inline MismatchCalculatorLine(const Eigen::MatrixBase<DerivedV> &_V,
                               const Eigen::MatrixBase<DerivedF> &_F,
                               const Eigen::MatrixBase<DerivedV> &_PD1,
                               const Eigen::MatrixBase<DerivedV> &_PD2
                               ):
        V(_V),
        F(_F),
        PD1(_PD1),
        PD2(_PD2)
    {
        igl::per_face_normals(V,F,N);
        V_border = igl::is_border_vertex(F);
        igl::vertex_triangle_adjacency(V,F,VF,VFi);
        igl::triangle_triangle_adjacency(F,TT,TTi);
    }

    inline void calculateMismatchLine(Eigen::PlainObjectBase<DerivedO> &Handle_MMatch)
    {
        Handle_MMatch.setConstant(F.rows(),3,-1);
        for (unsigned int i=0;i<F.rows();i++)
        {
            for (int j=0;j<3;j++)
            {
                if (i==TT(i,j) || TT(i,j) == -1)
                    Handle_MMatch(i,j)=0;
                else
                    Handle_MMatch(i,j) = mismatchByLine(i, TT(i, j));
            }
        }
    }

};
}


template <typename DerivedV, typename DerivedF, typename DerivedO>
IGL_INLINE void igl::line_field_mismatch(const Eigen::MatrixBase<DerivedV> &V,
                                         const Eigen::MatrixBase<DerivedF> &F,
                                         const Eigen::MatrixBase<DerivedV> &PD1,
                                         const bool isCombed,
                                         Eigen::PlainObjectBase<DerivedO> &mismatch)
{
    DerivedV PD1_combed;
    DerivedV PD2_combed;

    if (!isCombed)
        igl::comb_line_field(V,F,PD1,PD1_combed);
    else
    {
        PD1_combed = PD1;
    }
    Eigen::MatrixXd B1,B2,B3;
    igl::local_basis(V,F,B1,B2,B3);
    PD2_combed = igl::rotate_vectors(PD1_combed, Eigen::VectorXd::Constant(1,igl::PI/2), B1, B2);
    igl::MismatchCalculatorLine<DerivedV, DerivedF, DerivedO> sf(V, F, PD1_combed, PD2_combed);
    sf.calculateMismatchLine(mismatch);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
