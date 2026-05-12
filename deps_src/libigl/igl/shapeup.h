// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2017 Amir Vaxman <avaxman@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SHAPEUP_H
#define IGL_SHAPEUP_H

#include "min_quad_with_fixed.h"
#include "igl_inline.h"
#include "setdiff.h"
#include "cat.h"
#include <Eigen/Core>
#include <vector>
#include "PI.h"


//This file implements the following algorithm:

//Bouaziz et al.
//Shape-Up: Shaping Discrete Geometry with Projections
//Computer Graphics Forum (Proc. SGP) 31(5), 2012

namespace igl
{
  /// Parameters and precomputed data for ShapeUp
  ///
  /// \fileinfo
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

  // Alec: I'm not sure why these are using PlainObjectBase but then not
  // templating.
      
  /// Every function here defines a local projection for ShapeUp, and must have
  /// the following structure to qualify:
  ///
  /// @param[in] P  #P by 3 set of points, either the initial solution, or from
  ///   previous iteration.
  /// @param[in] SC  #Set by 1 cardinalities of sets in S
  /// @param[in] S  #Sets by max(SC) independent sets where the local projection
  ///   applies. Values beyond column SC(i)-1 in row S(i,:) are "don't care"
  /// @param[out] projP  #S by 3*max(SC) in format xyzxyzxyz,  where the
  ///   projected points correspond to each set in S in the same order.
  /// @return Return value appears to be ignored
  ///
  /// \fileinfo
  //typedef std::function<
  //  bool(
  //      const Eigen::MatrixBase<Eigen::MatrixXd>&, 
  //      const Eigen::MatrixBase<Eigen::VectorXi>&, 
  //      const Eigen::MatrixBase<Eigen::MatrixXi>&, 
  //      Eigen::PlainObjectBase<Eigen::MatrixXd>&)> 
  //  shapeup_projection_function;
  using 
    shapeup_projection_function
    =
    std::function<
    bool(
        const Eigen::MatrixBase<Eigen::MatrixXd>&, 
        const Eigen::MatrixBase<Eigen::VectorXi>&, 
        const Eigen::MatrixBase<Eigen::MatrixXi>&, 
        Eigen::PlainObjectBase<Eigen::MatrixXd>&)> ;
  /// This projection does nothing but render points into projP. Mostly used for
  /// "echoing" the global step
  ///
  /// @param[in] P  #P by 3 set of points, either the initial solution, or from
  ///   previous iteration.
  /// @param[in] SC  #Set by 1 cardinalities of sets in S
  /// @param[in] S  #Sets by max(SC) independent sets where the local projection
  ///   applies. Values beyond column SC(i)-1 in row S(i,:) are "don't care"
  /// @param[out] projP  #S by 3*max(SC) in format xyzxyzxyz,  where the
  ///   projected points correspond to each set in S in the same order.
  /// @return Return value appears to be ignored
  ///
  /// \fileinfo
  IGL_INLINE bool shapeup_identity_projection(
    const Eigen::MatrixBase<Eigen::MatrixXd>& P, 
    const Eigen::MatrixBase<Eigen::VectorXi>& SC, 
    const Eigen::MatrixBase<Eigen::MatrixXi>& S,  
    Eigen::PlainObjectBase<Eigen::MatrixXd>& projP);
  
  /// the projection assumes that the sets are vertices of polygons in cyclic
  /// order
  ///
  /// @param[in] P  #P by 3 set of points, either the initial solution, or from
  ///   previous iteration.
  /// @param[in] SC  #Set by 1 cardinalities of sets in S
  /// @param[in] S  #Sets by max(SC) independent sets where the local projection
  ///   applies. Values beyond column SC(i)-1 in row S(i,:) are "don't care"
  /// @param[out] projP  #S by 3*max(SC) in format xyzxyzxyz,  where the
  ///   projected points correspond to each set in S in the same order.
  /// @return Return value appears to be ignored
  ///
  /// \fileinfo
  IGL_INLINE bool shapeup_regular_face_projection(
    const Eigen::MatrixBase<Eigen::MatrixXd>& P, 
    const Eigen::MatrixBase<Eigen::VectorXi>& SC, 
    const Eigen::MatrixBase<Eigen::MatrixXi>& S,  
    Eigen::PlainObjectBase<Eigen::MatrixXd>& projP);
  /// This function precomputation the necessary matrices for the ShapeUp
  /// process, and prefactorizes them.
  ///
  /// @param[in] P  #P by 3 point positions
  /// @param[in] SC  #Set by 1 cardinalities of sets in S
  /// @param[in] S  #Sets by max(SC) independent sets where the local projection
  ///   applies. Values beyond column SC(i)-1 in row S(i,:) are "don't care"
  /// @param[in] E  #E by 2 the "edges" of the set P; used for the smoothness
  ///   energy.
  /// @param[in] b  #b by 1 boundary (fixed) vertices from P.
  /// @param[in] wShape  #Set by 1
  /// @param[in] wSmooth   #b by 1 weights for constraints from S and positional
  ///   constraints (used in the global step)
  /// @param[out] sudata struct ShapeupData the data necessary to solve the
  ///   system in shapeup_solve
  /// @return true if precomputation was successful, false otherwise
  ///
  /// \fileinfo
  template <
    typename DerivedP,
    typename DerivedSC,
    typename DerivedS,
    typename Derivedw>
  IGL_INLINE bool shapeup_precomputation(
    const Eigen::MatrixBase<DerivedP>& P,
    const Eigen::MatrixBase<DerivedSC>& SC,
    const Eigen::MatrixBase<DerivedS>& S,
    const Eigen::MatrixBase<DerivedS>& E,
    const Eigen::MatrixBase<DerivedSC>& b,
    const Eigen::MatrixBase<Derivedw>& wShape,
    const Eigen::MatrixBase<Derivedw>& wSmooth,
    ShapeupData & sudata);
  /// This function solve the shapeup project optimization. shapeup_precompute
  /// must be called before with the same sudata, or results are unpredictable
  ///
  /// @param[in] bc  #b by 3 fixed point values corresonding to "b" in sudata
  /// @param[in] local_projection  function pointer taking (P,SC,S,projP),
  ///             where the first three parameters are as defined, and "projP" is the output, as a #S by 3*max(SC) function in format xyzxyzxyz, and where it returns the projected points corresponding to each set in S in the same order.
  ///            NOTE: the input values in P0 don't need to correspond to prescribed values in bc; the iterations will project them automatically (by design).
  /// @param[in] P0  #P by 3 initial solution (point positions)
  /// @param[in] sudata  the ShapeUpData structure computed in shapeup_precomputation()
  /// @param[in] quietIterations  flagging if to output iteration information.
  /// @param[out] P  the solution to the problem, indices corresponding to P0.
  /// @returns true if the solver converged, false otherwise.
  ///
  /// \fileinfo
  template <
    typename DerivedP,
    typename DerivedSC,
    typename DerivedS>
  IGL_INLINE bool shapeup_solve(
    const Eigen::MatrixBase<DerivedP>& bc,
    const std::function<bool(const Eigen::MatrixBase<DerivedP>&, const Eigen::MatrixBase<DerivedSC>&, const Eigen::MatrixBase<DerivedS>&,  Eigen::PlainObjectBase<DerivedP>&)>& local_projection,
    const Eigen::MatrixBase<DerivedP>& P0,
    const ShapeupData & sudata,
    const bool quietIterations,
    Eigen::PlainObjectBase<DerivedP>& P);
  
}

#ifndef IGL_STATIC_LIBRARY
#include "shapeup.cpp"
#endif

#endif
