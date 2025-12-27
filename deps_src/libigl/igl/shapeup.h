// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Amir Vaxman <avaxman@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SHAPEUP_H
#define IGL_SHAPEUP_H

#include <igl/min_quad_with_fixed.h>
#include <igl/igl_inline.h>
#include <igl/setdiff.h>
#include <igl/cat.h>
#include <Eigen/Core>
#include <vector>
#include <igl/PI.h>


//This file implements the following algorithm:

//Boaziz et al.
//Shape-Up: Shaping Discrete Geometry with Projections
//Computer Graphics Forum (Proc. SGP) 31(5), 2012

namespace igl
{
  struct ShapeupData{
    //input data
    Eigen::MatrixXd P;
    Eigen::VectorXi SC;
    Eigen::MatrixXi S;
    Eigen::VectorXi b;
    int maxIterations; //referring to number of local-global pairs.
    double pTolerance;   //algorithm stops when max(|P_k-P_{k-1}|)<pTolerance.
    double shapeCoeff, closeCoeff, smoothCoeff;
          
    //Internally-used matrices
    Eigen::SparseMatrix<double> DShape, DClose, DSmooth, Q, A, At, W;
          
    min_quad_with_fixed_data<double> solver_data;
          
    ShapeupData():
    maxIterations(50),
    pTolerance(10e-6),
    shapeCoeff(1.0),
    closeCoeff(100.0),
    smoothCoeff(0.0){}
  };
      
  //Every function here defines a local projection for ShapeUp, and must have the following structure to qualify:
  //Input:
  //	P		#P by 3				the set of points, either the initial solution, or from previous iteration.
  //  SC		#Set by 1           cardinalities of sets in S
  //  S		#Sets by max(SC)    independent sets where the local projection applies. Values beyond column SC(i)-1 in row S(i,:) are "don't care"
  //Output:
  //	projP	#S by 3*max(SC) in format xyzxyzxyz,  where the projected points correspond to each set in S in the same order.
  typedef std::function<bool(const Eigen::PlainObjectBase<Eigen::MatrixXd>&, const Eigen::PlainObjectBase<Eigen::VectorXi>&, const Eigen::PlainObjectBase<Eigen::MatrixXi>&, Eigen::PlainObjectBase<Eigen::MatrixXd>&)> shapeup_projection_function;

  
  //This projection does nothing but render points into projP. Mostly used for "echoing" the global step
  IGL_INLINE bool shapeup_identity_projection(const Eigen::PlainObjectBase<Eigen::MatrixXd>& P, const Eigen::PlainObjectBase<Eigen::VectorXi>& SC, const Eigen::PlainObjectBase<Eigen::MatrixXi>& S,  Eigen::PlainObjectBase<Eigen::MatrixXd>& projP);
  
  //the projection assumes that the sets are vertices of polygons in cyclic order
  IGL_INLINE bool shapeup_regular_face_projection(const Eigen::PlainObjectBase<Eigen::MatrixXd>& P, const Eigen::PlainObjectBase<Eigen::VectorXi>& SC, const Eigen::PlainObjectBase<Eigen::MatrixXi>& S,  Eigen::PlainObjectBase<Eigen::MatrixXd>& projP);

    
  //This function precomputation the necessary matrices for the ShapeUp process, and prefactorizes them.
    
  //input:
  //  P   #P by 3             point positions
  //  SC  #Set by 1           cardinalities of sets in S
  //  S   #Sets by max(SC)    independent sets where the local projection applies. Values beyond column SC(i)-1 in row S(i,:) are "don't care"
  //  E   #E by 2             the "edges" of the set P; used for the smoothness energy.
  //  b   #b by 1             boundary (fixed) vertices from P.
  //  wShape,   #Set by 1
  //  wSmooth   #b by 1       weights for constraints from S and positional constraints (used in the global step)

  // Output:
  //  sudata struct ShapeupData     the data necessary to solve the system in shapeup_solve

  template <
  typename DerivedP,
  typename DerivedSC,
  typename DerivedS,
  typename Derivedw>
  IGL_INLINE bool shapeup_precomputation(const Eigen::PlainObjectBase<DerivedP>& P,
                                         const Eigen::PlainObjectBase<DerivedSC>& SC,
                                         const Eigen::PlainObjectBase<DerivedS>& S,
                                         const Eigen::PlainObjectBase<DerivedS>& E,
                                         const Eigen::PlainObjectBase<DerivedSC>& b,
                                         const Eigen::PlainObjectBase<Derivedw>& wShape,
                                         const Eigen::PlainObjectBase<Derivedw>& wSmooth,
                                         ShapeupData & sudata);
    
    
    
  //This function solve the shapeup project optimization. shapeup_precompute must be called before with the same sudata, or results are unpredictable
    
  //Input:
  //bc                #b by 3 fixed point values corresonding to "b" in sudata
  //local_projection  function pointer taking (P,SC,S,projP),
  // where the first three parameters are as defined, and "projP" is the output, as a #S by 3*max(SC) function in format xyzxyzxyz, and where it returns the projected points corresponding to each set in S in the same order.
  //NOTE: the input values in P0 don't need to correspond to prescribed values in bc; the iterations will project them automatically (by design).
  //P0                #P by 3 initial solution (point positions)
  //sudata            the ShapeUpData structure computed in shapeup_precomputation()
  //quietIterations   flagging if to output iteration information.

  //Output:
  //P                 the solution to the problem, indices corresponding to P0.
  template <
  typename DerivedP,
  typename DerivedSC,
  typename DerivedS>
  IGL_INLINE bool shapeup_solve(const Eigen::PlainObjectBase<DerivedP>& bc,
                                const std::function<bool(const Eigen::PlainObjectBase<DerivedP>&, const Eigen::PlainObjectBase<DerivedSC>&, const Eigen::PlainObjectBase<DerivedS>&,  Eigen::PlainObjectBase<DerivedP>&)>& local_projection,
                                const Eigen::PlainObjectBase<DerivedP>& P0,
                                const ShapeupData & sudata,
                                const bool quietIterations,
                                Eigen::PlainObjectBase<DerivedP>& P);
  
}

#ifndef IGL_STATIC_LIBRARY
#include "shapeup.cpp"
#endif

#endif
