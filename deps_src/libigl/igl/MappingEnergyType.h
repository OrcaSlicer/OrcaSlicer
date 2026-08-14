// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2018 Zhongshi Jiang <jiangzs@nyu.edu>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_MAPPINGENERGYTYPE_H
#define IGL_MAPPINGENERGYTYPE_H
namespace igl
{
  /// Energy Types used for Parameterization/Mapping. 
  /// Refer to SLIM [Rabinovich et al. 2017] for more details
  // Todo: Integrate with ARAPEnergyType
  enum MappingEnergyType
  {
    ARAP = 0,
    LOG_ARAP = 1,
    SYMMETRIC_DIRICHLET = 2,
    CONFORMAL = 3,
    EXP_CONFORMAL = 4,
    EXP_SYMMETRIC_DIRICHLET = 5,
    NUM_SLIM_ENERGY_TYPES = 6
  };
}
#endif
