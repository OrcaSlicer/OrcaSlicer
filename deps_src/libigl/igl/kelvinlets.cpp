// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can

#include "kelvinlets.h"
#include "PI.h"
#include "parallel_for.h"

namespace igl {

// Performs the deformation of a single point based on the regularized
// kelvinlets
//
// Inputs:
//   dt delta time used to calculate brush tip displacement
//   x  dim-vector of point to be deformed
//   x0 dim-vector of brush tip
//   f  dim-vector of brush force (translation)
//   F  dim by dim matrix of brush force matrix  (linear)
//   kp  parameters for the kelvinlet brush like brush radius, scale etc
// Returns:
//   X  dim-vector of the new point x gets displaced to post deformation
template<typename Derivedx,
         typename Derivedx0,
         typename Derivedf,
         typename DerivedF,
         typename Scalar>
IGL_INLINE auto kelvinlet_evaluator(const Scalar dt,
                                    const Eigen::MatrixBase<Derivedx>& x,
                                    const Eigen::MatrixBase<Derivedx0>& x0,
                                    const Eigen::MatrixBase<Derivedf>& f,
                                    const Eigen::MatrixBase<DerivedF>& F,
                                    const igl::KelvinletParams<Scalar>& kp)
  -> Eigen::Matrix<Scalar, 3, 1>
{
  static constexpr double POISSON_RATIO = 0.5;
  static constexpr double SHEAR_MODULUS = 1;
  static constexpr double a = 1 / (4 * igl::PI * SHEAR_MODULUS);
  static constexpr double b = a / (4 * (1 - POISSON_RATIO));
  static constexpr double c = 2 / (3 * a - 2 * b);

  const auto linearVelocity = f / c / kp.epsilon;

  const auto originAdjusted = x0 + linearVelocity * dt;
  const auto r = x - originAdjusted;
  const auto r_norm_sq = r.squaredNorm();

  std::function<Eigen::Matrix<Scalar, 3, 1>(const Scalar&)> kelvinlet;

  switch (kp.brushType) {
    case igl::BrushType::GRAB: {
      // Regularized Kelvinlets: Formula (6)
      kelvinlet = [&r, &f, &r_norm_sq](const Scalar& epsilon) {
        const auto r_epsilon = sqrt(r_norm_sq + epsilon * epsilon);
        const auto r_epsilon_3 = r_epsilon * r_epsilon * r_epsilon;
        auto t1 = ((a - b) / r_epsilon) * f;
        auto t2 = ((b / r_epsilon_3) * r * r.transpose()) * f;
        auto t3 = ((a * epsilon * epsilon) / (2 * r_epsilon_3)) * f;
        return t1 + t2 + t3;
      };
      break;
    }
    case igl::BrushType::TWIST: {
      // Regularized Kelvinlets: Formula (15)
      kelvinlet = [&r, &F, &r_norm_sq](const Scalar& epsilon) {
        const auto r_epsilon = sqrt(r_norm_sq + epsilon * epsilon);
        const auto r_epsilon_3 = r_epsilon * r_epsilon * r_epsilon;
        return -a *
               (1 / (r_epsilon_3) +
                3 * epsilon * epsilon /
                  (2 * r_epsilon_3 * r_epsilon * r_epsilon)) *
               F * r;
      };
      break;
    }
    case igl::BrushType::SCALE: {
      // Regularized Kelvinlets: Formula (16)
      kelvinlet = [&r, &F, &r_norm_sq](const Scalar& epsilon) {
        static constexpr auto b_compressible = a / 4; // assumes poisson ratio 0
        const auto r_epsilon = sqrt(r_norm_sq + epsilon * epsilon);
        const auto r_epsilon_3 = r_epsilon * r_epsilon * r_epsilon;
        auto coeff =
          (2 * b_compressible - a) *
          (1 / (r_epsilon_3) +
           3 * (epsilon * epsilon) / (2 * r_epsilon_3 * r_epsilon * r_epsilon));
        return coeff * F * r;
      };
      break;
    }
    case igl::BrushType::PINCH: {
      // Regularized Kelvinlets: Formula (17)
      kelvinlet = [&r, &F, &r_norm_sq, &kp](const Scalar& epsilon) {
        const auto r_epsilon = sqrt(r_norm_sq + kp.epsilon * kp.epsilon);
        const auto r_epsilon_3 = r_epsilon * r_epsilon * r_epsilon;
        auto t1 = ((2 * b - a) / r_epsilon_3) * F * r;
        auto t2_coeff = 3 / (2 * r_epsilon * r_epsilon * r_epsilon_3);
        auto t2 = t2_coeff * (2 * b * (r.transpose().dot(F * r)) * r +
                              a * epsilon * epsilon * epsilon * F * r);
        return t1 - t2;
      };
      break;
    }
  }

  if (kp.scale == 1) {
    return kelvinlet(kp.ep[0]);
  } else if (kp.scale == 2) {
    // Regularized Kelvinlets: Formula (8)
    return (kelvinlet(kp.ep[0]) - kelvinlet(kp.ep[1])) * 10;
  }
  // Regularized Kelvinlets: Formula (10)
  return (kp.w[0] * kelvinlet(kp.ep[0]) + kp.w[1] * kelvinlet(kp.ep[1]) +
          kp.w[2] * kelvinlet(kp.ep[2])) *
         20;
};

// Implements the Bogacki-Shrampine ODE Solver
// https://en.wikipedia.org/wiki/Bogacki%E2%80%93Shampine_method
//
// It calculates the second and third order approximations which can be used to
// estimate the error in the integration step
//
// Inputs:
//   t  starting time
//   dt delta time used to calculate brush tip displacement
//   x  dim-vector of point to be deformed
//   x0 dim-vector of brush tip
//   f  dim-vector of brush force (translation)
//   F  dim by dim matrix of brush force matrix  (linear)
//   kp parameters for the kelvinlet brush like brush radius, scale etc
// Outputs:
//   result dim vector holding the third order approximation result
//   error  The euclidean distance between the second and third order
//          approximations
template<typename Scalar,
         typename Derivedx,
         typename Derivedx0,
         typename Derivedf,
         typename DerivedF>
IGL_INLINE void integrate(const Scalar t,
                          const Scalar dt,
                          const Eigen::MatrixBase<Derivedx>& x,
                          const Eigen::MatrixBase<Derivedx0>& x0,
                          const Eigen::MatrixBase<Derivedf>& f,
                          const Eigen::MatrixBase<DerivedF>& F,
                          const igl::KelvinletParams<Scalar>& kp,
                          Eigen::MatrixBase<Derivedx>& result,
                          Scalar& error)
{
  constexpr Scalar a1 = 0;
  constexpr Scalar a2 = 1 / 2.0f;
  constexpr Scalar a3 = 3 / 4.0f;
  constexpr Scalar a4 = 1.0f;

  constexpr Scalar b21 = 1 / 2.0f;
  constexpr Scalar b31 = 0;
  constexpr Scalar b32 = 3 / 4.0f;
  constexpr Scalar b41 = 2 / 9.0f;
  constexpr Scalar b42 = 1 / 3.0f;
  constexpr Scalar b43 = 4 / 9.0f;

  constexpr Scalar c1 = 2 / 9.0f; // third order answer
  constexpr Scalar c2 = 1 / 3.0f;
  constexpr Scalar c3 = 4 / 9.0f;

  constexpr Scalar d1 = 7 / 24.0f; // second order answer
  constexpr Scalar d2 = 1 / 4.0f;
  constexpr Scalar d3 = 1 / 3.0f;
  constexpr Scalar d4 = 1 / 8.0f;

  auto k1 = dt * kelvinlet_evaluator(t + dt * a1, x, x0, f, F, kp);
  auto k2 = dt * kelvinlet_evaluator(t + dt * a2, x + k1 * b21, x0, f, F, kp);
  auto k3 = dt * kelvinlet_evaluator(
                   t + dt * a3, x + k1 * b31 + k2 * b32, x0, f, F, kp);
  auto k4 =
    dt * kelvinlet_evaluator(
           t + dt * a4, x + k1 * b41 + k2 * b42 + k3 * b43, x0, f, F, kp);
  auto r1 = x + k1 * d1 + k2 * d2 + k3 * d3 + k4 * d4;
  auto r2 = x + k1 * c1 + k2 * c2 + k3 * c3;
  result = r2;
  error = (r2 - r1).norm() / dt;
};

template<typename DerivedV,
         typename Derivedx0,
         typename Derivedf,
         typename DerivedF,
         typename DerivedU>
IGL_INLINE void kelvinlets(
  const Eigen::MatrixBase<DerivedV>& V,
  const Eigen::MatrixBase<Derivedx0>& x0,
  const Eigen::MatrixBase<Derivedf>& f,
  const Eigen::MatrixBase<DerivedF>& F,
  const KelvinletParams<typename DerivedV::Scalar>& params,
  Eigen::PlainObjectBase<DerivedU>& U)
{
  using Scalar = typename DerivedV::Scalar;
  constexpr auto max_error = 0.001f;
  constexpr Scalar safety = 0.9;

  const auto calc_displacement = [&](const int index) {
    Scalar dt = 0.1;
    Scalar t = 0;

    Eigen::Matrix<Scalar, 3, 1> x = V.row(index).transpose();
    decltype(x) result;
    Scalar error;
    // taking smaller steps seems to prevents weird inside-out artifacts in the
    // final result. This implementation used an adaptive time step solver to
    // numerically integrate the ODEs
    while (t < 1) {
      dt = std::min(dt, 1 - t);
      integrate(t, dt, x, x0, f, F, params, result, error);
      auto new_dt = dt * safety * std::pow(max_error / error, 1 / 3.0);
      if (error <= max_error || dt <= 0.001) {
        x = result;
        t += dt;
        dt = new_dt;
      } else {
        dt = std::max(abs(new_dt - dt) < 0.001 ? dt / 2.f : new_dt, 0.001);
      }
    }
    U.row(index) = x.transpose();
  };

  const int n = V.rows();
  U.resize(n, V.cols());
  igl::parallel_for(n, calc_displacement, 1000);
}
}
#ifdef IGL_STATIC_LIBRARY
template void igl::kelvinlets<Eigen::Matrix<double, -1, -1, 0, -1, -1>,
                              Eigen::Matrix<double, 3, 1, 0, 3, 1>,
                              Eigen::Matrix<double, 3, 1, 0, 3, 1>,
                              Eigen::Matrix<double, 3, 3, 0, 3, 3>,
                              Eigen::Matrix<double, -1, -1, 0, -1, -1>>(
  Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&,
  Eigen::MatrixBase<Eigen::Matrix<double, 3, 1, 0, 3, 1>> const&,
  Eigen::MatrixBase<Eigen::Matrix<double, 3, 1, 0, 3, 1>> const&,
  Eigen::MatrixBase<Eigen::Matrix<double, 3, 3, 0, 3, 3>> const&,
  igl::KelvinletParams<double> const&,
  Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>>&);
#endif
