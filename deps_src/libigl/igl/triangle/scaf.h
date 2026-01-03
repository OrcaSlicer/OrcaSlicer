// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Zhongshi Jiang <jiangzs@nyu.edu>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#ifndef IGL_SCAF_H
#define IGL_SCAF_H

#include "../slim.h"
#include "../igl_inline.h"
#include "../MappingEnergyType.h"

namespace igl
{
  namespace triangle
  {
    /// Use a similar interface to igl::slim
    /// Implement ready-to-use 2D version of the algorithm described in 
    /// SCAF: Simplicial Complex Augmentation Framework for Bijective Maps
    /// Zhongshi Jiang, Scott Schaefer, Daniele Panozzo, ACM Trancaction on Graphics (Proc. SIGGRAPH Asia 2017)
    /// For a complete implementation and customized UI, please refer to https://github.com/jiangzhongshi/scaffold-map
    struct SCAFData
    {
      double scaffold_factor = 10;
      igl::MappingEnergyType scaf_energy = igl::MappingEnergyType::SYMMETRIC_DIRICHLET;
      igl::MappingEnergyType slim_energy = igl::MappingEnergyType::SYMMETRIC_DIRICHLET;

      // Output
      int dim = 2;
      /// scaffold + isometric 
      double total_energy;
      /// objective value
      double energy;

      long mv_num = 0, mf_num = 0;
      long sv_num = 0, sf_num = 0;
      long v_num{}, f_num = 0;
      /// input initial mesh V
      Eigen::MatrixXd m_V;
      /// input initial mesh F/T
      Eigen::MatrixXi m_T;
      // INTERNAL
      /// whole domain uv: mesh + free vertices
      Eigen::MatrixXd w_uv;
      /// scaffold domain tets: scaffold tets
      Eigen::MatrixXi s_T;
      Eigen::MatrixXi w_T;

      /// mesh area or volume
      Eigen::VectorXd m_M;
      /// scaffold area or volume
      Eigen::VectorXd s_M;
      /// area/volume weights for whole
      Eigen::VectorXd w_M;
      /// area or volume
      double mesh_measure = 0;
      double proximal_p = 0;

      Eigen::VectorXi frame_ids;
      Eigen::VectorXi fixed_ids;

      std::map<int, Eigen::RowVectorXd> soft_cons;
      double soft_const_p = 1e4;

      Eigen::VectorXi internal_bnd;
      Eigen::MatrixXd rect_frame_V;
      // multi-chart support
      std::vector<int> component_sizes;
      std::vector<int> bnd_sizes;
    
      // reweightedARAP interior variables.
      bool has_pre_calc = false;
      Eigen::SparseMatrix<double> Dx_s, Dy_s, Dz_s;
      Eigen::SparseMatrix<double> Dx_m, Dy_m, Dz_m;
      Eigen::MatrixXd Ri_m, Ji_m, Ri_s, Ji_s;
      Eigen::MatrixXd W_m, W_s;
    };


    /// Compute necessary information to start using SCAF
    ///
    /// @param[in] V           #V by 3 list of mesh vertex positions
    /// @param[in] F           #F by 3 list of mesh triangles
    /// @param[in] V_init      #V by 2 list of initial mesh vertex positions
    /// @param[in,out] data  resulting precomputed data
    /// @param[in] slim_energy Energy type to minimize
    /// @param[in] b           list of boundary indices into V (soft constraint)
    /// @param[in] bc          #b by dim list of boundary conditions (soft constraint)
    /// @param[in] soft_p      Soft penalty factor (can be zero)
    ///
    /// \fileinfo
    IGL_INLINE void scaf_precompute(
        const Eigen::MatrixXd &V,
        const Eigen::MatrixXi &F,
        const Eigen::MatrixXd &V_init,
        const MappingEnergyType slim_energy,
        const Eigen::VectorXi& b,
        const Eigen::MatrixXd& bc,
        const double soft_p,
        triangle::SCAFData &data);

    /// Run iter_num iterations of SCAF, with precomputed data
    /// @param[in] data  precomputed data
    /// @param[in] iter_num  number of iterations to run
    /// @returns resulting V_o (in SLIMData): #V by dim list of mesh vertex positions
    IGL_INLINE Eigen::MatrixXd scaf_solve(const int iter_num, triangle::SCAFData &data);

    /// Set up the SCAF system L * uv = rhs, without solving it.
    /// @param[in] s:   igl::SCAFData. Will be modified by energy and Jacobian computation.
    /// @param[out] L:   m by m matrix
    /// @param[out] rhs: m by 1 vector
    ///         with m = dim * (#V_mesh + #V_scaf - #V_frame)
    IGL_INLINE void scaf_system(triangle::SCAFData &s, Eigen::SparseMatrix<double> &L, Eigen::VectorXd &rhs);

    namespace scaf
    {
      /// Compute SCAF energy
      /// @param[in] s:     igl::SCAFData
      /// @param[in] w_uv:  (#V_mesh + #V_scaf) by dim matrix
      /// @param[in] whole: Include scaffold if true
      /// @returns energy
      IGL_INLINE double compute_energy(SCAFData &s, const Eigen::MatrixXd &w_uv, bool whole);
    }
  }
}

#ifndef IGL_STATIC_LIBRARY
#  include "scaf.cpp"
#endif

#endif //IGL_SCAF_H
