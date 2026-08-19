// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Zhongshi Jiang <jiangzs@nyu.edu>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.

#include "mapping_energy_with_jacobians.h"
#include "polar_svd.h"

IGL_INLINE double igl::mapping_energy_with_jacobians(
  const Eigen::MatrixXd &Ji, 
  const Eigen::VectorXd &areas, 
  igl::MappingEnergyType slim_energy, 
  double exp_factor){

  double energy = 0;
  if (Ji.cols() == 4)
  {
    Eigen::Matrix<double, 2, 2> ji;
    for (int i = 0; i < Ji.rows(); i++)
    {
      ji(0, 0) = Ji(i, 0);
      ji(0, 1) = Ji(i, 1);
      ji(1, 0) = Ji(i, 2);
      ji(1, 1) = Ji(i, 3);

      typedef Eigen::Matrix<double, 2, 2> Mat2;
      typedef Eigen::Matrix<double, 2, 1> Vec2;
      Mat2 ri, ti, ui, vi;
      Vec2 sing;
      igl::polar_svd(ji, ri, ti, ui, sing, vi);
      double s1 = sing(0);
      double s2 = sing(1);

      switch (slim_energy)
      {
        case igl::MappingEnergyType::ARAP:
        {
          energy += areas(i) * (pow(s1 - 1, 2) + pow(s2 - 1, 2));
          break;
        }
        case igl::MappingEnergyType::SYMMETRIC_DIRICHLET:
        {
          energy += areas(i) * (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2));
          break;
        }
        case igl::MappingEnergyType::EXP_SYMMETRIC_DIRICHLET:
        {
          energy += areas(i) * exp(exp_factor * (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2)));
          break;
        }
        case igl::MappingEnergyType::LOG_ARAP:
        {
          energy += areas(i) * (pow(log(s1), 2) + pow(log(s2), 2));
          break;
        }
        case igl::MappingEnergyType::CONFORMAL:
        {
          energy += areas(i) * ((pow(s1, 2) + pow(s2, 2)) / (2 * s1 * s2));
          break;
        }
        case igl::MappingEnergyType::EXP_CONFORMAL:
        {
          energy += areas(i) * exp(exp_factor * ((pow(s1, 2) + pow(s2, 2)) / (2 * s1 * s2)));
          break;
        }
        default: assert(false);

      }

    }
  }
  else
  {
    Eigen::Matrix<double, 3, 3> ji;
    for (int i = 0; i < Ji.rows(); i++)
    {
      ji(0, 0) = Ji(i, 0);
      ji(0, 1) = Ji(i, 1);
      ji(0, 2) = Ji(i, 2);
      ji(1, 0) = Ji(i, 3);
      ji(1, 1) = Ji(i, 4);
      ji(1, 2) = Ji(i, 5);
      ji(2, 0) = Ji(i, 6);
      ji(2, 1) = Ji(i, 7);
      ji(2, 2) = Ji(i, 8);

      typedef Eigen::Matrix<double, 3, 3> Mat3;
      typedef Eigen::Matrix<double, 3, 1> Vec3;
      Mat3 ri, ti, ui, vi;
      Vec3 sing;
      igl::polar_svd(ji, ri, ti, ui, sing, vi);
      double s1 = sing(0);
      double s2 = sing(1);
      double s3 = sing(2);

      switch (slim_energy)
      {
        case igl::MappingEnergyType::ARAP:
        {
          energy += areas(i) * (pow(s1 - 1, 2) + pow(s2 - 1, 2) + pow(s3 - 1, 2));
          break;
        }
        case igl::MappingEnergyType::SYMMETRIC_DIRICHLET:
        {
          energy += areas(i) * (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2) + pow(s3, 2) + pow(s3, -2));
          break;
        }
        case igl::MappingEnergyType::EXP_SYMMETRIC_DIRICHLET:
        {
          energy += areas(i) * exp(exp_factor *
                                    (pow(s1, 2) + pow(s1, -2) + pow(s2, 2) + pow(s2, -2) + pow(s3, 2) + pow(s3, -2)));
          break;
        }
        case igl::MappingEnergyType::LOG_ARAP:
        {
          energy += areas(i) * (pow(log(s1), 2) + pow(log(std::abs(s2)), 2) + pow(log(std::abs(s3)), 2));
          break;
        }
        case igl::MappingEnergyType::CONFORMAL:
        {
          energy += areas(i) * ((pow(s1, 2) + pow(s2, 2) + pow(s3, 2)) / (3 * pow(s1 * s2 * s3, 2. / 3.)));
          break;
        }
        case igl::MappingEnergyType::EXP_CONFORMAL:
        {
          energy += areas(i) * exp(exp_factor * (pow(s1, 2) + pow(s2, 2) + pow(s3, 2)) / (3 * pow(s1 * s2 * s3, 2. / 3.)));
          break;
        }
        default: assert(false);
      }
    }
  }

  return energy;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
