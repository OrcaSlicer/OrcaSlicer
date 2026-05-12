// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "fit_cubic_bezier.h"
#include "bezier.h"
#include "EPS.h"

// Adapted from main.c accompanying
// An Algorithm for Automatically Fitting Digitized Curves
// by Philip J. Schneider
// from "Graphics Gems", Academic Press, 1990
IGL_INLINE void igl::fit_cubic_bezier(
  const Eigen::MatrixXd & d,
  const double error,
  std::vector<Eigen::MatrixXd> & cubics)
{
  const int nPts = d.rows();
  // Don't attempt to fit curve to single point
  if(nPts==1) { return; }
  // Avoid using zero tangent
  const static auto tangent = [](
    const Eigen::MatrixXd & d,
    const int i,const int dir)->Eigen::RowVectorXd
  {
    int j = i;
    const int nPts = d.rows();
    Eigen::RowVectorXd t;
    while(true)
    {
      // look at next point
      j += dir;
      if(j < 0 || j>=nPts)
      {
        // All points are coincident?
        // give up and use zero tangent...
        return Eigen::RowVectorXd::Zero(1,d.cols());
      }
      t = d.row(j)-d.row(i);
      if(t.squaredNorm() > igl::DOUBLE_EPS)
      {
        break;
      }
    }
    return t.normalized();
  };
  Eigen::RowVectorXd tHat1 = tangent(d,0,+1);
  Eigen::RowVectorXd tHat2 = tangent(d,nPts-1,-1);
  // If first and last points are identically equal, then consider closed
  const bool closed = (d.row(0) - d.row(d.rows()-1)).squaredNorm() == 0;
  // If closed loop make tangents match
  if(closed)
  {
    tHat1 = (tHat1 - tHat2).eval().normalized();
    tHat2 = -tHat1;
  }
  cubics.clear();
  fit_cubic_bezier_substring(d,0,nPts-1,tHat1,tHat2,error,closed,cubics);
};

IGL_INLINE void igl::fit_cubic_bezier_substring(
  const Eigen::MatrixXd & d,
  const int first,
  const int last,
  const Eigen::RowVectorXd & tHat1,
  const Eigen::RowVectorXd & tHat2,
  const double error,
  const bool force_split,
  std::vector<Eigen::MatrixXd> & cubics)
{
  // Helper functions
  // Evaluate a Bezier curve at a particular parameter value
  const static auto bezier_eval = [](const Eigen::MatrixXd & V, const double t)
    { Eigen::RowVectorXd P; bezier(V,t,P); return P; };
  //
  // Use Newton-Raphson iteration to find better root.
  const static auto NewtonRaphsonRootFind = [](
    const Eigen::MatrixXd & Q,
    const Eigen::RowVectorXd & P,
    const double u)->double
  {
    /* Compute Q(u)	*/
    Eigen::RowVectorXd Q_u = bezier_eval(Q, u);
    Eigen::MatrixXd Q1(3,Q.cols());
    Eigen::MatrixXd Q2(2,Q.cols());
    /* Generate control vertices for Q'	*/
    for (int i = 0; i <= 2; i++) 
    {
      Q1.row(i) = (Q.row(i+1) - Q.row(i)) * 3.0;
    }
    /* Generate control vertices for Q'' */
    for (int i = 0; i <= 1; i++) 
    {
      Q2.row(i) = (Q1.row(i+1) - Q1.row(i)) * 2.0;
    }
    /* Compute Q'(u) and Q''(u)	*/
    const Eigen::RowVectorXd Q1_u = bezier_eval(Q1, u);
    const Eigen::RowVectorXd Q2_u = bezier_eval(Q2, u);
    /* Compute f(u)/f'(u) */
    const double numerator = ((Q_u-P).array() * Q1_u.array()).array().sum();
    const double denominator = 
      Q1_u.squaredNorm() + ((Q_u-P).array() * Q2_u.array()).array().sum();
    /* u = u - f(u)/f'(u) */
    return u - (numerator/denominator);
  };
  const static auto ComputeMaxError = [](
    const Eigen::MatrixXd & d,
    const int first,
    const int last,
    const Eigen::MatrixXd & bezCurve,
    const Eigen::VectorXd & u,
    int & splitPoint)->double
  {
    Eigen::VectorXd E(last - (first+1));
    splitPoint = (last-first + 1)/2;
    double maxDist = 0.0;
    for (int i = first + 1; i < last; i++) 
    {
      Eigen::RowVectorXd P = bezier_eval(bezCurve, u(i-first));
      const double dist = (P-d.row(i)).squaredNorm();
      E(i-(first+1)) = dist;
      if (dist >= maxDist)
      {
        maxDist = dist;
        // Worst offender
        splitPoint = i;
      }
    }
    //const double half_total = E.array().sum()/2;
    //double run = 0;
    //for (int i = first + 1; i < last; i++) 
    //{
    //  run += E(i-(first+1));
    //  if(run>half_total)
    //  {
    //    // When accumulated ½ the error --> more symmetric, but requires more
    //    // curves
    //    splitPoint = i;
    //    break;
    //  }
    //}
    return maxDist;
  };
  const static auto Straight = [](
    const Eigen::MatrixXd & d,
    const int first,
    const int last,
    const Eigen::RowVectorXd & tHat1,
    const Eigen::RowVectorXd & tHat2,
    Eigen::MatrixXd & bezCurve)
  {
    bezCurve.resize(4,d.cols());
    const double dist = (d.row(last)-d.row(first)).norm()/3.0;
    bezCurve.row(0) = d.row(first);
    bezCurve.row(1) = d.row(first) + tHat1*dist;
    bezCurve.row(2) = d.row(last) + tHat2*dist;
    bezCurve.row(3) = d.row(last);
  };
  const static auto GenerateBezier = [](
    const Eigen::MatrixXd & d,
    const int first,
    const int last,
    const Eigen::VectorXd & uPrime,
    const Eigen::RowVectorXd & tHat1,
    const Eigen::RowVectorXd & tHat2,
    Eigen::MatrixXd & bezCurve)
  {
    bezCurve.resize(4,d.cols());
    const int nPts = last - first + 1;
    const static auto B0 = [](const double u)->double
      { double tmp = 1.0 - u; return (tmp * tmp * tmp);};
    const static auto B1 = [](const double u)->double
      { double tmp = 1.0 - u; return (3 * u * (tmp * tmp));};
    const static auto B2 = [](const double u)->double
      { double tmp = 1.0 - u; return (3 * u * u * tmp); };
    const static auto B3 = [](const double u)->double
      { return (u * u * u); };
    /* Compute the A's	*/
    std::vector<std::vector<Eigen::RowVectorXd> > A(nPts);
    for (int i = 0; i < nPts; i++)
    {
      Eigen::RowVectorXd v1 = tHat1*B1(uPrime(i));
      Eigen::RowVectorXd v2 = tHat2*B2(uPrime(i));
      A[i] = {v1,v2};
    }
    /* Create the C and X matrices	*/
    Eigen::MatrixXd C(2,2);
    Eigen::VectorXd X(2);
    C(0,0) = 0.0;
    C(0,1) = 0.0;
    C(1,0) = 0.0;
    C(1,1) = 0.0;
    X(0)    = 0.0;
    X(1)    = 0.0;
    for( int i = 0; i < nPts; i++) 
    {
      C(0,0) += A[i][0].dot(A[i][0]);
      C(0,1) += A[i][0].dot(A[i][1]);
      C(1,0) = C(0,1);
      C(1,1) += A[i][1].dot(A[i][1]);
      const Eigen::RowVectorXd tmp = 
        d.row(first+i)-(
              d.row(first)*B0(uPrime(i))+
              d.row(first)*B1(uPrime(i))+
              d.row(last)*B2(uPrime(i))+
              d.row(last)*B3(uPrime(i)));
  	X(0) += A[i][0].dot(tmp);
  	X(1) += A[i][1].dot(tmp);
      }
    /* Compute the determinants of C and X	*/
    double det_C0_C1 = C(0,0) * C(1,1) - C(1,0) * C(0,1);
    const double det_C0_X  = C(0,0) * X(1)    - C(0,1) * X(0);
    const double det_X_C1  = X(0)    * C(1,1) - X(1)    * C(0,1);
    /* Finally, derive alpha values	*/
    if (det_C0_C1 == 0.0) 
    {
      det_C0_C1 = (C(0,0) * C(1,1)) * 10e-12;
    }
    const double alpha_l = det_X_C1 / det_C0_C1;
    const double alpha_r = det_C0_X / det_C0_C1;
    /*  If alpha negative, use the Wu/Barsky heuristic (see text) */
        /* (if alpha is 0, you get coincident control points that lead to
         * divide by zero in any subsequent NewtonRaphsonRootFind() call. */
    if (alpha_l < 1.0e-6 || alpha_r < 1.0e-6) 
    {
      return Straight(d,first,last,tHat1,tHat2,bezCurve);
    }
    bezCurve.row(0) = d.row(first);
    bezCurve.row(1) = d.row(first) + tHat1*alpha_l;
    bezCurve.row(2) = d.row(last) + tHat2*alpha_r;
    bezCurve.row(3) = d.row(last);
  };

  const int maxIterations = 4;
  // This is a bad idea if error<1 ...
  //const double iterationError = error * error;
  const double iterationError = 100 * error;
  const int nPts = last - first + 1;
  /*  Use heuristic if region only has two points in it */
  if(nPts == 2)
  {
    Eigen::MatrixXd bezCurve;
    Straight(d,first,last,tHat1,tHat2,bezCurve);
    cubics.push_back(bezCurve);
    return;
  }
  // ChordLengthParameterize
  Eigen::VectorXd u(last-first+1);
  u(0) = 0;
  for (int i = first+1; i <= last; i++)
  {
    u(i-first) = u(i-first-1) + (d.row(i)-d.row(i-1)).norm();
  }
  for (int i = first + 1; i <= last; i++) 
  {
    u(i-first) = u(i-first) / u(last-first);
  }
  Eigen::MatrixXd bezCurve;
  GenerateBezier(d, first, last, u, tHat1, tHat2, bezCurve);


  int splitPoint;
  double maxError = ComputeMaxError(d, first, last, bezCurve, u, splitPoint);
  if (!force_split && maxError < error)
  {
    cubics.push_back(bezCurve);
    return;
  }
  /*  If error not too large, try some reparameterization  */
  /*  and iteration */
  if (maxError < iterationError)
  {
    for (int i = 0; i < maxIterations; i++) 
    {
      Eigen::VectorXd uPrime;
      // Reparameterize
      uPrime.resize(last-first+1);
      for (int i = first; i <= last; i++) 
      {
        uPrime(i-first) = NewtonRaphsonRootFind(bezCurve, d.row(i), u(i- first));
      }
      GenerateBezier(d, first, last, uPrime, tHat1, tHat2, bezCurve);
      maxError = ComputeMaxError(d, first, last, bezCurve, uPrime, splitPoint);
      if (!force_split && maxError < error) {
        cubics.push_back(bezCurve);
        return;
      }
      u = uPrime;
    }
  }

  /* Fitting failed -- split at max error point and fit recursively */
  const Eigen::RowVectorXd tHatCenter = 
    (d.row(splitPoint-1)-d.row(splitPoint+1)).normalized();
  //foobar
  fit_cubic_bezier_substring(
    d,first,splitPoint,tHat1,tHatCenter,error,false,cubics);
  fit_cubic_bezier_substring(
    d,splitPoint,last,(-tHatCenter).eval(),tHat2,error,false,cubics);
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
