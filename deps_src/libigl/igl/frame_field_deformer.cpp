// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "frame_field_deformer.h"

#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <vector>

#include "cotmatrix_entries.h"
#include "cotmatrix.h"
#include "vertex_triangle_adjacency.h"

namespace igl
{

class Frame_field_deformer
{
public:

  IGL_INLINE Frame_field_deformer();
  IGL_INLINE ~Frame_field_deformer();

  // Initialize the optimizer
  IGL_INLINE void init(const Eigen::MatrixXd& _V, const Eigen::MatrixXi& _F, const Eigen::MatrixXd& _D1, const Eigen::MatrixXd& _D2, double _Lambda, double _perturb_rotations, int _fixed = 1);

  // Run N optimization steps
  IGL_INLINE void optimize(int N, bool reset = false);

  // Reset optimization
  IGL_INLINE void reset_opt();

  // Precomputation of all components
  IGL_INLINE void precompute_opt();

  // Precomputation for deformation energy
  IGL_INLINE void precompute_ARAP(Eigen::SparseMatrix<double> & Lff, Eigen::MatrixXd & LfcVc);

  // Precomputation for regularization
  IGL_INLINE void precompute_SMOOTH(Eigen::SparseMatrix<double> & MS, Eigen::MatrixXd & bS);

  // extracts a r x c block from sparse matrix mat into sparse matrix m1
  // (r0,c0) is upper left entry of block
  IGL_INLINE void extractBlock(Eigen::SparseMatrix<double> & mat, int r0, int c0, int r, int c, Eigen::SparseMatrix<double> & m1);

  // computes optimal rotations for faces of m wrt current coords in mw.V
  // returns a 3x3 matrix
  IGL_INLINE void compute_optimal_rotations();

  // global optimization step - linear system
  IGL_INLINE void compute_optimal_positions();

  // compute the output XField from deformation gradient
  IGL_INLINE void computeXField(std::vector< Eigen::Matrix<double,3,2> > & XF);

  // computes in WW the ideal warp at each tri to make the frame field a cross
  IGL_INLINE void compute_idealWarp(std::vector< Eigen::Matrix<double,3,3> > & WW);

  // -------------------------------- Variables ----------------------------------------------------

  // Mesh I/O:

  Eigen::MatrixXd V;                         // Original mesh - vertices
  Eigen::MatrixXi F;                         // Original mesh - faces

  std::vector<std::vector<int> > VT;                   // Vertex to triangle topology
  std::vector<std::vector<int> > VTi;                  // Vertex to triangle topology

  Eigen::MatrixXd V_w;                       // Warped mesh - vertices

  std::vector< Eigen::Matrix<double,3,2> > FF;  	// frame field FF in 3D (parallel to m.F)
  std::vector< Eigen::Matrix<double,3,3> > WW;    // warping matrices to make a cross field (parallel to m.F)
  std::vector< Eigen::Matrix<double,3,2> > XF;  	// pseudo-cross field from solution (parallel to m.F)

  int fixed;

  double perturb_rotations; // perturbation to rotation matrices

  // Numerics
  int nfree,nconst;					          // number of free/constrained vertices in the mesh - default all-but-1/1
  Eigen::MatrixXd C;							            // cotangent matrix of m
  Eigen::SparseMatrix<double> L;					          // Laplacian matrix of m

  Eigen::SparseMatrix<double> M;					          // matrix for global optimization - pre-conditioned
  Eigen::MatrixXd RHS;						            // pre-computed part of known term in global optimization
  std::vector< Eigen::Matrix<double,3,3> > RW;    // optimal rotation-warping matrices (parallel to m.F) -- INCORPORATES WW
  Eigen::SimplicialCholesky<Eigen::SparseMatrix<double> > solver;   // solver for linear system in global opt.

  // Parameters
private:
  double Lambda = 0.1;				        // weight of energy regularization

};

  IGL_INLINE Frame_field_deformer::Frame_field_deformer() {}

  IGL_INLINE Frame_field_deformer::~Frame_field_deformer() {}

  IGL_INLINE void Frame_field_deformer::init(const Eigen::MatrixXd& _V,
                          const Eigen::MatrixXi& _F,
                          const Eigen::MatrixXd& _D1,
                          const Eigen::MatrixXd& _D2,
                          double _Lambda,
                          double _perturb_rotations,
                          int _fixed)
{
  V = _V;
  F = _F;

  assert(_D1.rows() == _D2.rows());

  FF.clear();
  for (unsigned i=0; i < _D1.rows(); ++i)
  {
    Eigen::Matrix<double,3,2> ff;
    ff.col(0) = _D1.row(i);
    ff.col(1) = _D2.row(i);
    FF.push_back(ff);
  }

  fixed = _fixed;
  Lambda = _Lambda;
  perturb_rotations = _perturb_rotations;

  reset_opt();
  precompute_opt();
}


IGL_INLINE void Frame_field_deformer::optimize(int N, bool reset)
{
  //Reset optimization
	if (reset)
    reset_opt();

	// Iterative Local/Global optimization
  for (int i=0; i<N;i++)
  {
    compute_optimal_rotations();
    compute_optimal_positions();
		computeXField(XF);
  }
}

IGL_INLINE void Frame_field_deformer::reset_opt()
{
  V_w = V;

  for (unsigned i=0; i<V_w.rows(); ++i)
    for (unsigned j=0; j<V_w.cols(); ++j)
      V_w(i,j) += (double(rand())/double(RAND_MAX))*10e-4*perturb_rotations;

}

// precomputation of all components
IGL_INLINE void Frame_field_deformer::precompute_opt()
{
  using namespace Eigen;
	nfree = V.rows() - fixed;						    // free vertices (at the beginning ov m.V) - global
  nconst = V.rows()-nfree;						// #constrained vertices
  igl::vertex_triangle_adjacency(V,F,VT,VTi);                // compute vertex to face relationship

  igl::cotmatrix_entries(V,F,C);							     // cotangent matrix for opt. rotations - global

  igl::cotmatrix(V,F,L);

	SparseMatrix<double> MA;						// internal matrix for ARAP-warping energy
	MatrixXd LfcVc;										  // RHS (partial) for ARAP-warping energy
	SparseMatrix<double> MS;						// internal matrix for smoothing energy
	MatrixXd bS;										    // RHS (full) for smoothing energy

	precompute_ARAP(MA,LfcVc);					// precompute terms for the ARAP-warp part
	precompute_SMOOTH(MS,bS);						// precompute terms for the smoothing part
	compute_idealWarp(WW);              // computes the ideal warps
  RW.resize(F.rows());								// init rotation matrices - global

  M =	  (1-Lambda)*MA + Lambda*MS;		// matrix for linear system - global

	RHS = (1-Lambda)*LfcVc + Lambda*bS;	// RHS (partial) for linear system - global
  solver.compute(M);									// system pre-conditioning
  if (solver.info()!=Eigen::Success) {fprintf(stderr,"Decomposition failed in pre-conditioning!\n"); exit(-1);}

	fprintf(stdout,"Preconditioning done.\n");

}

IGL_INLINE void Frame_field_deformer::precompute_ARAP(Eigen::SparseMatrix<double> & Lff, Eigen::MatrixXd & LfcVc)
{
  using namespace Eigen;
	fprintf(stdout,"Precomputing ARAP terms\n");
	SparseMatrix<double> LL = -4*L;
	Lff = SparseMatrix<double>(nfree,nfree);
  extractBlock(LL,0,0,nfree,nfree,Lff);
	SparseMatrix<double> Lfc = SparseMatrix<double>(nfree,nconst);
  extractBlock(LL,0,nfree,nfree,nconst,Lfc);
	LfcVc = - Lfc * V_w.block(nfree,0,nconst,3);
}

IGL_INLINE void Frame_field_deformer::precompute_SMOOTH(Eigen::SparseMatrix<double> & MS, Eigen::MatrixXd & bS)
{
  using namespace Eigen;
	fprintf(stdout,"Precomputing SMOOTH terms\n");

	SparseMatrix<double> LL = 4*L*L;

  // top-left
	MS = SparseMatrix<double>(nfree,nfree);
  extractBlock(LL,0,0,nfree,nfree,MS);

  // top-right
	SparseMatrix<double> Mfc = SparseMatrix<double>(nfree,nconst);
  extractBlock(LL,0,nfree,nfree,nconst,Mfc);

	MatrixXd MfcVc = Mfc * V_w.block(nfree,0,nconst,3);
	bS = (LL*V).block(0,0,nfree,3)-MfcVc;

}

  IGL_INLINE void Frame_field_deformer::extractBlock(Eigen::SparseMatrix<double> & mat, int r0, int c0, int r, int c, Eigen::SparseMatrix<double> & m1)
{
  std::vector<Eigen::Triplet<double> > tripletList;
  for (int k=c0; k<c0+c; ++k)
    for (Eigen::SparseMatrix<double>::InnerIterator it(mat,k); it; ++it)
    {
      if (it.row()>=r0 && it.row()<r0+r)
        tripletList.push_back(Eigen::Triplet<double>(it.row()-r0,it.col()-c0,it.value()));
    }
  m1.setFromTriplets(tripletList.begin(), tripletList.end());
}

IGL_INLINE void Frame_field_deformer::compute_optimal_rotations()
{
  using namespace Eigen;
  Matrix<double,3,3> r,S,P,PP,D;

  for (int i=0;i<F.rows();i++)
	{
		// input tri --- could be done once and saved in a matrix
		P.col(0) = (V.row(F(i,1))-V.row(F(i,0))).transpose();
		P.col(1) = (V.row(F(i,2))-V.row(F(i,1))).transpose();
		P.col(2) = (V.row(F(i,0))-V.row(F(i,2))).transpose();

		P = WW[i] * P;		// apply ideal warp

		// current tri
		PP.col(0) = (V_w.row(F(i,1))-V_w.row(F(i,0))).transpose();
		PP.col(1) = (V_w.row(F(i,2))-V_w.row(F(i,1))).transpose();
		PP.col(2) = (V_w.row(F(i,0))-V_w.row(F(i,2))).transpose();

		// cotangents
		D <<    C(i,2), 0,      0,
    0,      C(i,0), 0,
    0,      0,      C(i,1);

		S = PP*D*P.transpose();
		Eigen::JacobiSVD<Matrix<double,3,3> > svd(S, Eigen::ComputeFullU | Eigen::ComputeFullV );
		Matrix<double,3,3>  su = svd.matrixU();
		Matrix<double,3,3>  sv = svd.matrixV();
		r = su*sv.transpose();

		if (r.determinant()<0)  // correct reflections
		{
			su(0,2)=-su(0,2); su(1,2)=-su(1,2); su(2,2)=-su(2,2);
			r = su*sv.transpose();
		}
		RW[i] = r*WW[i];		// RW INCORPORATES IDEAL WARP WW!!!
	}
}

IGL_INLINE void Frame_field_deformer::compute_optimal_positions()
{
  using namespace Eigen;
	// compute variable RHS of ARAP-warp part of the system
  MatrixXd b(nfree,3);          // fx3 known term of the system
	MatrixXd X;										// result
  int t;		  									// triangles incident to edge (i,j)
	int vi,i1,i2;									// index of vertex i wrt tri t0

  for (int i=0;i<nfree;i++)
  {
    b.row(i) << 0.0, 0.0, 0.0;
    for (int k=0;k<(int)VT[i].size();k++)					// for all incident triangles
    {
      t = VT[i][k];												// incident tri
			vi = (i==F(t,0))?0:(i==F(t,1))?1:(i==F(t,2))?2:3;	// index of i in t
			assert(vi!=3);
			i1 = F(t,(vi+1)%3);
			i2 = F(t,(vi+2)%3);
			b.row(i)+=(C(t,(vi+2)%3)*RW[t]*(V.row(i1)-V.row(i)).transpose()).transpose();
			b.row(i)+=(C(t,(vi+1)%3)*RW[t]*(V.row(i2)-V.row(i)).transpose()).transpose();
    }
  }
  b/=2.0;
	b=-4*b;

	b*=(1-Lambda);		// blend

  b+=RHS;				// complete known term

	X = solver.solve(b);
	if (solver.info()!=Eigen::Success) {printf("Solving linear system failed!\n"); return;}

	// copy result to mw.V
  for (int i=0;i<nfree;i++)
    V_w.row(i)=X.row(i);

}

  IGL_INLINE void Frame_field_deformer::computeXField(std::vector< Eigen::Matrix<double,3,2> > & XF)
{
  using namespace Eigen;
  Matrix<double,3,3> P,PP,DG;
	XF.resize(F.rows());

  for (int i=0;i<F.rows();i++)
	{
		int i0,i1,i2;
		// indexes of vertices of face i
		i0 = F(i,0); i1 = F(i,1); i2 = F(i,2);

		// input frame
		P.col(0) = (V.row(i1)-V.row(i0)).transpose();
		P.col(1) = (V.row(i2)-V.row(i0)).transpose();
		P.col(2) = P.col(0).cross(P.col(1));

		// output triangle brought to origin
		PP.col(0) = (V_w.row(i1)-V_w.row(i0)).transpose();
		PP.col(1) = (V_w.row(i2)-V_w.row(i0)).transpose();
		PP.col(2) = PP.col(0).cross(PP.col(1));

		// deformation gradient
		DG = PP * P.inverse();
		XF[i] = DG * FF[i];
	}
}

// computes in WW the ideal warp at each tri to make the frame field a cross
  IGL_INLINE void Frame_field_deformer::compute_idealWarp(std::vector< Eigen::Matrix<double,3,3> > & WW)
{
  using namespace Eigen;

  WW.resize(F.rows());
	for (int i=0;i<(int)FF.size();i++)
	{
		Vector3d v0,v1,v2;
		v0 = FF[i].col(0);
		v1 = FF[i].col(1);
		v2=v0.cross(v1); v2.normalize();			// normal

		Matrix3d A,AI;								// compute affine map A that brings:
		A <<    v0[0], v1[0], v2[0],				//	first vector of FF to x unary vector
    v0[1], v1[1], v2[1],				//	second vector of FF to xy plane
    v0[2], v1[2], v2[2];				//	triangle normal to z unary vector
		AI = A.inverse();

		// polar decomposition to discard rotational component (unnecessary but makes it easier)
		Eigen::JacobiSVD<Matrix<double,3,3> > svd(AI, Eigen::ComputeFullU | Eigen::ComputeFullV );
		//Matrix<double,3,3>  au = svd.matrixU();
		Matrix<double,3,3>  av = svd.matrixV();
		DiagonalMatrix<double,3>	as(svd.singularValues());
		WW[i] = av*as*av.transpose();
	}
}

}


IGL_INLINE void igl::frame_field_deformer(
  const Eigen::MatrixXd& V,
  const Eigen::MatrixXi& F,
  const Eigen::MatrixXd& FF1,
  const Eigen::MatrixXd& FF2,
  Eigen::MatrixXd&       V_d,
  Eigen::MatrixXd&       FF1_d,
  Eigen::MatrixXd&       FF2_d,
  const int              iterations,
  const double           lambda,
  const bool             perturb_initial_guess)
{
  using namespace Eigen;
  // Solvers
  Frame_field_deformer deformer;

  // Init optimizer
  deformer.init(V, F, FF1, FF2, lambda, perturb_initial_guess ? 0.1 : 0);

  // Optimize
  deformer.optimize(iterations,true);

  // Copy positions
  V_d = deformer.V_w;

  // Allocate
  FF1_d.resize(F.rows(),3);
  FF2_d.resize(F.rows(),3);

  // Copy frame field
  for(unsigned i=0; i<deformer.XF.size(); ++i)
  {
    FF1_d.row(i) = deformer.XF[i].col(0);
    FF2_d.row(i) = deformer.XF[i].col(1);
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
