// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2018 Zhongshi Jiang <jiangzs@nyu.edu>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "scaf.h"
#include "triangulate.h"

#include <Eigen/Dense>
#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <Eigen/SparseQR>
#include "../PI.h"
#include "../Timer.h"
#include "../boundary_loop.h"
#include "../cat.h"
#include "../IGL_ASSERT.h"
#include "../doublearea.h"
#include "../flip_avoiding_line_search.h"
#include "../flipped_triangles.h"
#include "../grad.h"
#include "../harmonic.h"
#include "../local_basis.h"
#include "../map_vertices_to_circle.h"
#include "../polar_svd.h"
#include "../slice.h"
#include "../slice_into.h"
#include "../slim.h"
#include "../placeholders.h"
#include "../mapping_energy_with_jacobians.h"

#include <map>
#include <algorithm>
#include <set>
#include <vector>
namespace igl
{
namespace triangle
{
namespace scaf
{
IGL_INLINE void update_scaffold(igl::triangle::SCAFData &s)
{
  s.mv_num = s.m_V.rows();
  s.mf_num = s.m_T.rows();

  s.v_num = s.w_uv.rows();
  s.sf_num = s.s_T.rows();

  s.sv_num = s.v_num - s.mv_num;
  s.f_num = s.sf_num + s.mf_num;

  s.s_M = Eigen::VectorXd::Constant(s.sf_num, s.scaffold_factor);
}

IGL_INLINE void adjusted_grad(Eigen::MatrixXd &V,
                   Eigen::MatrixXi &F,
                   double area_threshold,
                   Eigen::SparseMatrix<double> &Dx,
                   Eigen::SparseMatrix<double> &Dy,
                   Eigen::SparseMatrix<double> &Dz)
{
  Eigen::VectorXd M;
  igl::doublearea(V, F, M);
  std::vector<int> degen;
  for (int i = 0; i < M.size(); i++)
    if (M(i) < area_threshold)
      degen.push_back(i);

  Eigen::SparseMatrix<double> G;
  igl::grad(V, F, G);

  Dx = G.topRows(F.rows());
  Dy = G.block(F.rows(), 0, F.rows(), V.rows());
  Dz = G.bottomRows(F.rows());

  // handcraft uniform gradient for faces area falling below threshold.
  double sin60 = std::sin(igl::PI / 3);
  double cos60 = std::cos(igl::PI / 3);
  double deno = std::sqrt(sin60 * area_threshold);
  Eigen::MatrixXd standard_grad(3, 3);
  standard_grad << -sin60 / deno, sin60 / deno, 0,
      -cos60 / deno, -cos60 / deno, 1 / deno,
      0, 0, 0;

  for (auto k : degen)
    for (int j = 0; j < 3; j++)
    {
      Dx.coeffRef(k, F(k, j)) = standard_grad(0, j);
      Dy.coeffRef(k, F(k, j)) = standard_grad(1, j);
      Dz.coeffRef(k, F(k, j)) = standard_grad(2, j);
    }
}

IGL_INLINE void compute_scaffold_gradient_matrix(SCAFData &s,
                                      Eigen::SparseMatrix<double> &D1,
                                      Eigen::SparseMatrix<double> &D2)
{
  using namespace Eigen;
  Eigen::SparseMatrix<double> G;
  MatrixXi F_s = s.s_T;
  int vn = s.v_num;
  MatrixXd V = MatrixXd::Zero(vn, 3);
  V.leftCols(2) = s.w_uv;

  double min_bnd_edge_len = INFINITY;
  int acc_bnd = 0;
  for (int i = 0; i < s.bnd_sizes.size(); i++)
  {
    int current_size = s.bnd_sizes[i];

    for (int e = acc_bnd; e < acc_bnd + current_size - 1; e++)
    {
      min_bnd_edge_len = (std::min)(min_bnd_edge_len,
                                    (s.w_uv.row(s.internal_bnd(e)) -
                                     s.w_uv.row(s.internal_bnd(e + 1)))
                                        .squaredNorm());
    }
    min_bnd_edge_len = (std::min)(min_bnd_edge_len,
                                  (s.w_uv.row(s.internal_bnd(acc_bnd)) -
                                   s.w_uv.row(s.internal_bnd(acc_bnd + current_size - 1)))
                                      .squaredNorm());
    acc_bnd += current_size;
  }

  double area_threshold = min_bnd_edge_len / 4.0;
  Eigen::SparseMatrix<double> Dx, Dy, Dz;
  adjusted_grad(V, F_s, area_threshold, Dx, Dy, Dz);

  MatrixXd F1, F2, F3;
  igl::local_basis(V, F_s, F1, F2, F3);
  D1 = F1.col(0).asDiagonal() * Dx + F1.col(1).asDiagonal() * Dy +
       F1.col(2).asDiagonal() * Dz;
  D2 = F2.col(0).asDiagonal() * Dx + F2.col(1).asDiagonal() * Dy +
       F2.col(2).asDiagonal() * Dz;
}

IGL_INLINE void mesh_improve(igl::triangle::SCAFData &s)
{
  using namespace Eigen;
  MatrixXd m_uv = s.w_uv.topRows(s.mv_num);
  MatrixXd V_bnd;
  V_bnd.resize(s.internal_bnd.size(), 2);
  for (int i = 0; i < s.internal_bnd.size(); i++) // redoing step 1.
  {
    V_bnd.row(i) = m_uv.row(s.internal_bnd(i));
  }

  if (s.rect_frame_V.size() == 0)
  {
    Matrix2d ob; // = rect_corners;
    {
      VectorXd uv_max = m_uv.colwise().maxCoeff();
      VectorXd uv_min = m_uv.colwise().minCoeff();
      VectorXd uv_mid = (uv_max + uv_min) / 2.;

      Eigen::Array2d scaf_range(3, 3);
      ob.row(0) = uv_mid.array() + scaf_range * ((uv_min - uv_mid).array());
      ob.row(1) = uv_mid.array() + scaf_range * ((uv_max - uv_mid).array());
    }
    Vector2d rect_len;
    rect_len << ob(1, 0) - ob(0, 0), ob(1, 1) - ob(0, 1);
    int frame_points = 5;

    s.rect_frame_V.resize(4 * frame_points, 2);
    for (int i = 0; i < frame_points; i++)
    {
      // 0,0;0,1
      s.rect_frame_V.row(i) << ob(0, 0), ob(0, 1) + i * rect_len(1) / frame_points;
      // 0,0;1,1
      s.rect_frame_V.row(i + frame_points)
          << ob(0, 0) + i * rect_len(0) / frame_points,
          ob(1, 1);
      // 1,0;1,1
      s.rect_frame_V.row(i + 2 * frame_points) << ob(1, 0), ob(1, 1) - i * rect_len(1) / frame_points;
      // 1,0;0,1
      s.rect_frame_V.row(i + 3 * frame_points)
          << ob(1, 0) - i * rect_len(0) / frame_points,
          ob(0, 1);
      // 0,0;0,1
    }
    s.frame_ids = Eigen::VectorXi::LinSpaced(s.rect_frame_V.rows(), s.mv_num, s.mv_num + s.rect_frame_V.rows());
  }

  // Concatenate Vert and Edge
  MatrixXd V;
  MatrixXi E;
  igl::cat(1, V_bnd, s.rect_frame_V, V);
  E.resize(V.rows(), 2);
  for (int i = 0; i < E.rows(); i++)
    E.row(i) << i, i + 1;
  int acc_bs = 0;
  for (auto bs : s.bnd_sizes)
  {
    E(acc_bs + bs - 1, 1) = acc_bs;
    acc_bs += bs;
  }
  E(V.rows() - 1, 1) = acc_bs;
  assert(acc_bs == s.internal_bnd.size());

  MatrixXd H = MatrixXd::Zero(s.component_sizes.size(), 2);
  {
    int hole_f = 0;
    int hole_i = 0;
    for (auto cs : s.component_sizes)
    {
      for (int i = 0; i < 3; i++)
        H.row(hole_i) += m_uv.row(s.m_T(hole_f, i)); // redoing step 2
      hole_f += cs;
      hole_i++;
    }
  }
  H /= 3.;

  MatrixXd uv2;
  igl::triangle::triangulate(V, E, H, std::basic_string<char>("qYYQ"), uv2, s.s_T);
  auto bnd_n = s.internal_bnd.size();

  for (auto i = 0; i < s.s_T.rows(); i++)
    for (auto j = 0; j < s.s_T.cols(); j++)
    {
      auto &x = s.s_T(i, j);
      if (x < bnd_n)
        x = s.internal_bnd(x);
      else
        x += m_uv.rows() - bnd_n;
    }

  igl::cat(1, s.m_T, s.s_T, s.w_T);
  s.w_uv.conservativeResize(m_uv.rows() - bnd_n + uv2.rows(), 2);
  s.w_uv.bottomRows(uv2.rows() - bnd_n) = uv2.bottomRows(-bnd_n + uv2.rows());

  update_scaffold(s);

  // after_mesh_improve
  compute_scaffold_gradient_matrix(s, s.Dx_s, s.Dy_s);

  s.Dx_s.makeCompressed();
  s.Dy_s.makeCompressed();
  s.Dz_s.makeCompressed();
  s.Ri_s = MatrixXd::Zero(s.Dx_s.rows(), s.dim * s.dim);
  s.Ji_s.resize(s.Dx_s.rows(), s.dim * s.dim);
  s.W_s.resize(s.Dx_s.rows(), s.dim * s.dim);
}

IGL_INLINE void add_new_patch(igl::triangle::SCAFData &s, const Eigen::MatrixXd &V_ref,
                   const Eigen::MatrixXi &F_ref,
                   const Eigen::RowVectorXd &/*center*/,
                   const Eigen::MatrixXd &uv_init)
{
  using namespace std;
  using namespace Eigen;

  assert(uv_init.rows() != 0);
  Eigen::VectorXd M;
  igl::doublearea(V_ref, F_ref, M);
  s.mesh_measure += M.sum() / 2;

  Eigen::VectorXi bnd;
  Eigen::MatrixXd bnd_uv;

  std::vector<std::vector<int>> all_bnds;
  igl::boundary_loop(F_ref, all_bnds);

  s.component_sizes.push_back(F_ref.rows());

  MatrixXd m_uv = s.w_uv.topRows(s.mv_num);
  igl::cat(1, m_uv, uv_init, s.w_uv);

  s.m_M.conservativeResize(s.mf_num + M.size());
  s.m_M.bottomRows(M.size()) = M / 2;

  for (auto cur_bnd : all_bnds)
  {
    s.internal_bnd.conservativeResize(s.internal_bnd.size() + cur_bnd.size());
    s.internal_bnd.bottomRows(cur_bnd.size()) = Map<ArrayXi>(cur_bnd.data(), cur_bnd.size()) + s.mv_num;
    s.bnd_sizes.push_back(cur_bnd.size());
  }

  s.m_T.conservativeResize(s.mf_num + F_ref.rows(), 3);
  s.m_T.bottomRows(F_ref.rows()) = F_ref.array() + s.mv_num;
  s.mf_num += F_ref.rows();

  s.m_V.conservativeResize(s.mv_num + V_ref.rows(), 3);
  s.m_V.bottomRows(V_ref.rows()) = V_ref;
  s.mv_num += V_ref.rows();

  s.rect_frame_V = MatrixXd();

  mesh_improve(s);
}

IGL_INLINE void compute_jacobians(SCAFData &s, const Eigen::MatrixXd &V_new, bool whole)
{
  auto comp_J2 = [](const Eigen::MatrixXd &uv,
                    const Eigen::SparseMatrix<double> &Dx,
                    const Eigen::SparseMatrix<double> &Dy,
                    Eigen::MatrixXd &Ji) {
    // Ji=[D1*u,D2*u,D1*v,D2*v];
    Ji.resize(Dx.rows(), 4);
    Ji.col(0) = Dx * uv.col(0);
    Ji.col(1) = Dy * uv.col(0);
    Ji.col(2) = Dx * uv.col(1);
    Ji.col(3) = Dy * uv.col(1);
  };

  Eigen::MatrixXd m_V_new = V_new.topRows(s.mv_num);
  comp_J2(m_V_new, s.Dx_m, s.Dy_m, s.Ji_m);
  if (whole)
    comp_J2(V_new, s.Dx_s, s.Dy_s, s.Ji_s);
}

IGL_INLINE double compute_energy_from_jacobians(const Eigen::MatrixXd &Ji,
                                     const Eigen::VectorXd &areas,
                                     igl::MappingEnergyType energy_type)
{
  double energy = 0;
  if (energy_type == igl::MappingEnergyType::SYMMETRIC_DIRICHLET)
    energy = -4; // comply with paper description
  return energy + igl::mapping_energy_with_jacobians(Ji, areas, energy_type, 0);
}

IGL_INLINE double compute_soft_constraint_energy(const SCAFData &s)
{
  double e = 0;
  for (auto const &x : s.soft_cons)
    e += s.soft_const_p * (x.second - s.w_uv.row(x.first)).squaredNorm();

  return e;
}

IGL_INLINE double compute_energy(SCAFData &s, const Eigen::MatrixXd &w_uv, bool whole)
{
  if (w_uv.rows() != s.v_num)
    assert(!whole);
  compute_jacobians(s, w_uv, whole);
  double energy = compute_energy_from_jacobians(s.Ji_m, s.m_M, s.slim_energy);

  if (whole)
    energy += compute_energy_from_jacobians(s.Ji_s, s.s_M, s.scaf_energy);
  energy += compute_soft_constraint_energy(s);
  return energy;
}

IGL_INLINE void buildAm(const Eigen::VectorXd &sqrt_M,
             const Eigen::SparseMatrix<double> &Dx,
             const Eigen::SparseMatrix<double> &Dy,
             const Eigen::MatrixXd &W,
             Eigen::SparseMatrix<double> &Am)
{
  std::vector<Eigen::Triplet<double>> IJV;
  Eigen::SparseMatrix<double> Dz;

  Eigen::SparseMatrix<double> MDx = sqrt_M.asDiagonal() * Dx;
  Eigen::SparseMatrix<double> MDy = sqrt_M.asDiagonal() * Dy;
  igl::slim_buildA(MDx, MDy, Dz, W, IJV);

  Am.setFromTriplets(IJV.begin(), IJV.end());
  Am.makeCompressed();
}

IGL_INLINE void buildRhs(const Eigen::VectorXd &sqrt_M,
              const Eigen::MatrixXd &W,
              const Eigen::MatrixXd &Ri,
              Eigen::VectorXd &f_rhs)
{
  const int dim = (W.cols() == 4) ? 2 : 3;
  const int f_n = W.rows();
  f_rhs.resize(dim * dim * f_n);

  for (int i = 0; i < f_n; i++)
  {
    auto sqrt_area = sqrt_M(i);
    f_rhs(i + 0 * f_n) = sqrt_area * (W(i, 0) * Ri(i, 0) + W(i, 1) * Ri(i, 1));
    f_rhs(i + 1 * f_n) = sqrt_area * (W(i, 0) * Ri(i, 2) + W(i, 1) * Ri(i, 3));
    f_rhs(i + 2 * f_n) = sqrt_area * (W(i, 2) * Ri(i, 0) + W(i, 3) * Ri(i, 1));
    f_rhs(i + 3 * f_n) = sqrt_area * (W(i, 2) * Ri(i, 2) + W(i, 3) * Ri(i, 3));
  }
}

IGL_INLINE void get_complement(const Eigen::VectorXi &bnd_ids, int v_n, Eigen::ArrayXi &unknown_ids)
{ // get the complement of bnd_ids.
  int assign = 0, i = 0;
  for (int get = 0; i < v_n && get < bnd_ids.size(); i++)
  {
    if (bnd_ids(get) == i)
      get++;
    else
      unknown_ids(assign++) = i;
  }
  while (i < v_n)
    unknown_ids(assign++) = i++;
  assert(assign + bnd_ids.size() == v_n);
}

IGL_INLINE void build_surface_linear_system(const SCAFData &s, Eigen::SparseMatrix<double> &L, Eigen::VectorXd &rhs)
{
  using namespace Eigen;
  using namespace std;

  const int v_n = s.v_num - (s.frame_ids.size());
  const int dim = s.dim;
  const int f_n = s.mf_num;

  // to get the  complete A
  Eigen::VectorXd sqrtM = s.m_M.array().sqrt();
  Eigen::SparseMatrix<double> A(dim * dim * f_n, dim * v_n);
  auto decoy_Dx_m = s.Dx_m;
  decoy_Dx_m.conservativeResize(s.W_m.rows(), v_n);
  auto decoy_Dy_m = s.Dy_m;
  decoy_Dy_m.conservativeResize(s.W_m.rows(), v_n);
  buildAm(sqrtM, decoy_Dx_m, decoy_Dy_m, s.W_m, A);

  const VectorXi &bnd_ids = s.fixed_ids;
  auto bnd_n = bnd_ids.size();
  if (bnd_n == 0)
  {

    Eigen::SparseMatrix<double> At = A.transpose();
    At.makeCompressed();

    Eigen::SparseMatrix<double> id_m(At.rows(), At.rows());
    id_m.setIdentity();

    L = At * A;

    Eigen::VectorXd frhs;
    buildRhs(sqrtM, s.W_m, s.Ri_m, frhs);
    rhs = At * frhs;
  }
  else
  {
    MatrixXd bnd_pos = s.w_uv(bnd_ids, igl::placeholders::all);

    ArrayXi known_ids(bnd_ids.size() * dim);
    ArrayXi unknown_ids((v_n - bnd_ids.rows()) * dim);
    get_complement(bnd_ids, v_n, unknown_ids);
    VectorXd known_pos(bnd_ids.size() * dim);
    for (int d = 0; d < dim; d++)
    {
      auto n_b = bnd_ids.rows();
      known_ids.segment(d * n_b, n_b) = bnd_ids.array() + d * v_n;
      known_pos.segment(d * n_b, n_b) = bnd_pos.col(d);
      unknown_ids.block(d * (v_n - n_b), 0, v_n - n_b, unknown_ids.cols()) =
          unknown_ids.topRows(v_n - n_b) + d * v_n;
    }

    Eigen::SparseMatrix<double> Au, Ae;
    igl::slice(A, unknown_ids, 2, Au);
    igl::slice(A, known_ids, 2, Ae);

    Eigen::SparseMatrix<double> Aut = Au.transpose();
    Aut.makeCompressed();

    L = Aut * Au;

    Eigen::VectorXd frhs;
    buildRhs(sqrtM, s.W_m, s.Ri_m, frhs);

    rhs = Aut * (frhs - Ae * known_pos);
  }

  // add soft constraints.
  for (auto const &x : s.soft_cons)
  {
    int v_idx = x.first;

    for (int d = 0; d < dim; d++)
    {
      rhs(d * (v_n) + v_idx) += s.soft_const_p * x.second(d); // rhs
      L.coeffRef(d * v_n + v_idx,
                 d * v_n + v_idx) += s.soft_const_p; // diagonal
    }
  }
}

IGL_INLINE void build_scaffold_linear_system(const SCAFData &s, Eigen::SparseMatrix<double> &L, Eigen::VectorXd &rhs)
{
  using namespace Eigen;

  const int f_n = s.W_s.rows();
  const int v_n = s.Dx_s.cols();
  const int dim = s.dim;

  Eigen::VectorXd sqrtM = s.s_M.array().sqrt();
  Eigen::SparseMatrix<double> A(dim * dim * f_n, dim * v_n);
  buildAm(sqrtM, s.Dx_s, s.Dy_s, s.W_s, A);

  VectorXi bnd_ids;
  igl::cat(1, s.fixed_ids, s.frame_ids, bnd_ids);

  auto bnd_n = bnd_ids.size();
  IGL_ASSERT(bnd_n > 0);
  MatrixXd bnd_pos = s.w_uv(bnd_ids, igl::placeholders::all);

  ArrayXi known_ids(bnd_ids.size() * dim);
  ArrayXi unknown_ids((v_n - bnd_ids.rows()) * dim);

  get_complement(bnd_ids, v_n, unknown_ids);

  VectorXd known_pos(bnd_ids.size() * dim);
  for (int d = 0; d < dim; d++)
  {
    auto n_b = bnd_ids.rows();
    known_ids.segment(d * n_b, n_b) = bnd_ids.array() + d * v_n;
    known_pos.segment(d * n_b, n_b) = bnd_pos.col(d);
    unknown_ids.block(d * (v_n - n_b), 0, v_n - n_b, unknown_ids.cols()) =
        unknown_ids.topRows(v_n - n_b) + d * v_n;
  }
  Eigen::VectorXd sqrt_M = s.s_M.array().sqrt();

  // manual slicing for A(:, unknown/known)'
  Eigen::SparseMatrix<double> Au, Ae;
  igl::slice(A, unknown_ids, 2, Au);
  igl::slice(A, known_ids, 2, Ae);

  Eigen::SparseMatrix<double> Aut = Au.transpose();
  Aut.makeCompressed();

  L = Aut * Au;

  Eigen::VectorXd frhs;
  buildRhs(sqrtM, s.W_s, s.Ri_s, frhs);

  rhs = Aut * (frhs - Ae * known_pos);
}

IGL_INLINE void build_weighted_arap_system(SCAFData &s, Eigen::SparseMatrix<double> &L, Eigen::VectorXd &rhs)
{
  // fixed frame solving:
  // x_e as the fixed frame, x_u for unknowns (mesh + unknown scaffold)
  // min ||(A_u*x_u + A_e*x_e) - b||^2
  // => A_u'*A_u*x_u  = Au'* (b - A_e*x_e) := Au'* b_u
  //
  // separate matrix build:
  // min ||A_m x_m - b_m||^2 + ||A_s x_all - b_s||^2 + soft + proximal
  // First change dimension of A_m to fit for x_all
  // (Not just at the end, since x_all is flattened along dimensions)
  // L = A_m'*A_m + A_s'*A_s + soft + proximal
  // rhs = A_m'* b_m + A_s' * b_s + soft + proximal
  //
  Eigen::SparseMatrix<double> L_m, L_s;
  Eigen::VectorXd rhs_m, rhs_s;
  build_surface_linear_system(s, L_m, rhs_m);  // complete Am, with soft
  build_scaffold_linear_system(s, L_s, rhs_s); // complete As, without proximal

  L = L_m + L_s;
  rhs = rhs_m + rhs_s;
  L.makeCompressed();
}

IGL_INLINE void solve_weighted_arap(SCAFData &s, Eigen::MatrixXd &uv)
{
  using namespace Eigen;
  using namespace std;
  int dim = s.dim;
  igl::Timer timer;
  timer.start();

  VectorXi bnd_ids;
  igl::cat(1, s.fixed_ids, s.frame_ids, bnd_ids);
  const auto v_n = s.v_num;
  const auto bnd_n = bnd_ids.size();
  assert(bnd_n > 0);
  MatrixXd bnd_pos = s.w_uv(bnd_ids, igl::placeholders::all);

  ArrayXi known_ids(bnd_n * dim);
  ArrayXi unknown_ids((v_n - bnd_n) * dim);

  get_complement(bnd_ids, v_n, unknown_ids);

  VectorXd known_pos(bnd_ids.size() * dim);
  for (int d = 0; d < dim; d++)
  {
    auto n_b = bnd_ids.rows();
    known_ids.segment(d * n_b, n_b) = bnd_ids.array() + d * v_n;
    known_pos.segment(d * n_b, n_b) = bnd_pos.col(d);
    unknown_ids.block(d * (v_n - n_b), 0, v_n - n_b, unknown_ids.cols()) =
        unknown_ids.topRows(v_n - n_b) + d * v_n;
  }

  Eigen::SparseMatrix<double> L;
  Eigen::VectorXd rhs;
  build_weighted_arap_system(s, L, rhs);

  Eigen::VectorXd unknown_Uc((v_n - s.frame_ids.size() - s.fixed_ids.size()) * dim), Uc(dim * v_n);

  SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
  unknown_Uc = solver.compute(L).solve(rhs);
  Uc(unknown_ids) = unknown_Uc;
  Uc(known_ids) = known_pos;

  uv = Map<Matrix<double, -1, -1, Eigen::ColMajor>>(Uc.data(), v_n, dim);
}

IGL_INLINE double perform_iteration(SCAFData &s)
{
  Eigen::MatrixXd V_out = s.w_uv;
  compute_jacobians(s, V_out, true);
  igl::slim_update_weights_and_closest_rotations_with_jacobians(s.Ji_m, s.slim_energy, 0, s.W_m, s.Ri_m);
  igl::slim_update_weights_and_closest_rotations_with_jacobians(s.Ji_s, s.scaf_energy, 0, s.W_s, s.Ri_s);
  solve_weighted_arap(s, V_out);
  std::function<double(Eigen::MatrixXd&)> whole_E = [&s](Eigen::MatrixXd &uv) { return compute_energy(s, uv, true); };

  Eigen::MatrixXi w_T;
  if (s.m_T.cols() == s.s_T.cols())
    igl::cat(1, s.m_T, s.s_T, w_T);
  else
    w_T = s.s_T;
  return igl::flip_avoiding_line_search( w_T, s.w_uv, V_out, whole_E, -1) /
    s.mesh_measure;
}

}
}
}

IGL_INLINE void igl::triangle::scaf_precompute(
    const Eigen::MatrixXd &V,
    const Eigen::MatrixXi &F,
    const Eigen::MatrixXd &V_init,
    const igl::MappingEnergyType slim_energy,
    const Eigen::VectorXi &b,
    const Eigen::MatrixXd &bc,
    const double soft_p,
    igl::triangle::SCAFData &data)
{
  Eigen::MatrixXd CN;
  Eigen::MatrixXi FN;
  igl::triangle::scaf::add_new_patch(data, V, F, Eigen::RowVector2d(0, 0), V_init);
  data.soft_const_p = soft_p;
  for (int i = 0; i < b.rows(); i++)
    data.soft_cons[b(i)] = bc.row(i);
  data.slim_energy = slim_energy;

  auto &s = data;

  if (!data.has_pre_calc)
  {
    int dim = s.dim;
    Eigen::MatrixXd F1, F2, F3;
    igl::local_basis(s.m_V, s.m_T, F1, F2, F3);
    auto face_proj = [](Eigen::MatrixXd& F){
      std::vector<Eigen::Triplet<double> >IJV;
      int f_num = F.rows();
      for(int i=0; i<F.rows(); i++) {
        IJV.push_back(Eigen::Triplet<double>(i, i, F(i,0)));
        IJV.push_back(Eigen::Triplet<double>(i, i+f_num, F(i,1)));
        IJV.push_back(Eigen::Triplet<double>(i, i+2*f_num, F(i,2)));
      }
      Eigen::SparseMatrix<double> P(f_num, 3*f_num);
      P.setFromTriplets(IJV.begin(), IJV.end());
      return P;
    };
    Eigen::SparseMatrix<double> G;
    igl::grad(s.m_V, s.m_T, G);
    s.Dx_m = face_proj(F1) * G;
    s.Dy_m = face_proj(F2) * G;

    igl::triangle::scaf::compute_scaffold_gradient_matrix(s, s.Dx_s, s.Dy_s);

    s.Dx_m.makeCompressed();
    s.Dy_m.makeCompressed();
    s.Ri_m = Eigen::MatrixXd::Zero(s.Dx_m.rows(), dim * dim);
    s.Ji_m.resize(s.Dx_m.rows(), dim * dim);
    s.W_m.resize(s.Dx_m.rows(), dim * dim);

    s.Dx_s.makeCompressed();
    s.Dy_s.makeCompressed();
    s.Ri_s = Eigen::MatrixXd::Zero(s.Dx_s.rows(), dim * dim);
    s.Ji_s.resize(s.Dx_s.rows(), dim * dim);
    s.W_s.resize(s.Dx_s.rows(), dim * dim);

    data.has_pre_calc = true;
  }
}

IGL_INLINE Eigen::MatrixXd igl::triangle::scaf_solve(const int iter_num, igl::triangle::SCAFData &s)
{
  using namespace std;
  using namespace Eigen;
  s.energy = igl::triangle::scaf::compute_energy(s, s.w_uv, false) / s.mesh_measure;

  for (int it = 0; it < iter_num; it++)
  {
    s.total_energy = igl::triangle::scaf::compute_energy(s, s.w_uv, true) / s.mesh_measure;
    s.rect_frame_V = Eigen::MatrixXd();
    igl::triangle::scaf::mesh_improve(s);

    double new_weight = s.mesh_measure * s.energy / (s.sf_num * 100);
    s.scaffold_factor = new_weight;
    igl::triangle::scaf::update_scaffold(s);

    s.total_energy = igl::triangle::scaf::perform_iteration(s);

    s.energy =
        igl::triangle::scaf::compute_energy(s, s.w_uv, false) / s.mesh_measure;
  }

  return s.w_uv.topRows(s.mv_num);
}

IGL_INLINE void igl::triangle::scaf_system(igl::triangle::SCAFData &s, Eigen::SparseMatrix<double> &L, Eigen::VectorXd &rhs)
{
    s.energy = igl::triangle::scaf::compute_energy(s, s.w_uv, false) / s.mesh_measure;

    s.total_energy = igl::triangle::scaf::compute_energy(s, s.w_uv, true) / s.mesh_measure;
    s.rect_frame_V = Eigen::MatrixXd();
    igl::triangle::scaf::mesh_improve(s);

    double new_weight = s.mesh_measure * s.energy / (s.sf_num * 100);
    s.scaffold_factor = new_weight;
    igl::triangle::scaf::update_scaffold(s);

    igl::triangle::scaf::compute_jacobians(s, s.w_uv, true);
    igl::slim_update_weights_and_closest_rotations_with_jacobians(s.Ji_m, s.slim_energy, 0, s.W_m, s.Ri_m);
    igl::slim_update_weights_and_closest_rotations_with_jacobians(s.Ji_s, s.scaf_energy, 0, s.W_s, s.Ri_s);

    igl::triangle::scaf::build_weighted_arap_system(s, L, rhs);
}

#ifdef IGL_STATIC_LIBRARY
#endif
