// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2013 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "arap_dof.h"

#include "cotmatrix.h"
#include "massmatrix.h"
#include "speye.h"
#include "repdiag.h"
#include "repmat.h"
#include "slice.h"
#include "colon.h"
#include "is_sparse.h"
#include "mode.h"
#include "is_symmetric.h"
#include "group_sum_matrix.h"
#include "arap_rhs.h"
#include "covariance_scatter_matrix.h"
#include "fit_rotations.h"

#include "verbose.h"
#include "print_ijv.h"

#include "get_seconds_hires.h"
//#include "MKLEigenInterface.h"
#include "min_quad_dense.h"
#include "get_seconds.h"
#include "columnize.h"

// defined if no early exit is supported, i.e., always take a fixed number of iterations
#define IGL_ARAP_DOF_FIXED_ITERATIONS_COUNT

// A careful derivation of this implementation is given in the corresponding
// matlab function arap_dof.m
template <typename LbsMatrixType, typename SSCALAR>
IGL_INLINE bool igl::arap_dof_precomputation(
  const Eigen::MatrixXd & V, 
  const Eigen::MatrixXi & F,
  const LbsMatrixType & M,
  const Eigen::Matrix<int,Eigen::Dynamic,1> & G,
  ArapDOFData<LbsMatrixType, SSCALAR> & data)
{
  using namespace Eigen;
  typedef Matrix<SSCALAR, Dynamic, Dynamic> MatrixXS;
  // number of mesh (domain) vertices
  int n = V.rows();
  // cache problem size
  data.n = n;
  // dimension of mesh
  data.dim = V.cols();
  assert(data.dim == M.rows()/n);
  assert(data.dim*n == M.rows());
  if(data.dim == 3)
  {
    // Check if z-coordinate is all zeros
    if(V.col(2).minCoeff() == 0 && V.col(2).maxCoeff() == 0)
    {
      data.effective_dim = 2;
    }
  }else
  {
    data.effective_dim = data.dim;
  }
  // Number of handles
  data.m = M.cols()/data.dim/(data.dim+1);
  assert(data.m*data.dim*(data.dim+1) == M.cols());
  //assert(m == C.rows());

  //printf("n=%d; dim=%d; m=%d;\n",n,data.dim,data.m);

  // Build cotangent laplacian
  SparseMatrix<double> Lcot;
  //printf("cotmatrix()\n");
  cotmatrix(V,F,Lcot);
  // Discrete laplacian (should be minus matlab version)
  SparseMatrix<double> Lapl = -2.0*Lcot;
#ifdef EXTREME_VERBOSE
  cout<<"LaplIJV=["<<endl;print_ijv(Lapl,1);cout<<endl<<"];"<<
    endl<<"Lapl=sparse(LaplIJV(:,1),LaplIJV(:,2),LaplIJV(:,3),"<<
    Lapl.rows()<<","<<Lapl.cols()<<");"<<endl;
#endif

  // Get group sum scatter matrix, when applied sums all entries of the same
  // group according to G
  SparseMatrix<double> G_sum;
  if(G.size() == 0)
  {
    speye(n,G_sum);
  }else
  {
    // groups are defined per vertex, convert to per face using mode
    Eigen::Matrix<int,Eigen::Dynamic,1> GG;
    if(data.energy == ARAP_ENERGY_TYPE_ELEMENTS)
    {
      MatrixXi GF(F.rows(),F.cols());
      for(int j = 0;j<F.cols();j++)
      {
        Matrix<int,Eigen::Dynamic,1> GFj;
        slice(G,F.col(j),GFj);
        GF.col(j) = GFj;
      }
      mode<int>(GF,2,GG);
    }else
    {
      GG=G;
    }
    //printf("group_sum_matrix()\n");
    group_sum_matrix(GG,G_sum);
  }

#ifdef EXTREME_VERBOSE
  cout<<"G_sumIJV=["<<endl;print_ijv(G_sum,1);cout<<endl<<"];"<<
    endl<<"G_sum=sparse(G_sumIJV(:,1),G_sumIJV(:,2),G_sumIJV(:,3),"<<
    G_sum.rows()<<","<<G_sum.cols()<<");"<<endl;
#endif

  // Get covariance scatter matrix, when applied collects the covariance matrices
  // used to fit rotations to during optimization
  SparseMatrix<double> CSM;
  //printf("covariance_scatter_matrix()\n");
  covariance_scatter_matrix(V,F,data.energy,CSM);
#ifdef EXTREME_VERBOSE
  cout<<"CSMIJV=["<<endl;print_ijv(CSM,1);cout<<endl<<"];"<<
    endl<<"CSM=sparse(CSMIJV(:,1),CSMIJV(:,2),CSMIJV(:,3),"<<
    CSM.rows()<<","<<CSM.cols()<<");"<<endl;
#endif
  

  // Build the covariance matrix "constructor". This is a set of *scatter*
  // matrices that when multiplied on the right by column of the transformation
  // matrix entries (the degrees of freedom) L, we get a stack of dim by 1
  // covariance matrix column, with a column in the stack for each rotation
  // *group*. The output is a list of matrices because we construct each column
  // in the stack of covariance matrices with an independent matrix-vector
  // multiplication.
  //
  // We want to build S which is a stack of dim by dim covariance matrices.
  // Thus S is dim*g by dim, where dim is the number of dimensions and g is the
  // number of groups. We can precompute dim matrices CSM_M such that column i
  // in S is computed as S(:,i) = CSM_M{i} * L, where L is a column of the
  // skinning transformation matrix values. To be clear, the covariance matrix
  // for group k is then given as the dim by dim matrix pulled from the stack:
  // S((k-1)*dim + 1:dim,:)

  // Apply group sum to each dimension's block of covariance scatter matrix
  SparseMatrix<double> G_sum_dim;
  repdiag(G_sum,data.dim,G_sum_dim);
  CSM = (G_sum_dim * CSM).eval();
#ifdef EXTREME_VERBOSE
  cout<<"CSMIJV=["<<endl;print_ijv(CSM,1);cout<<endl<<"];"<<
    endl<<"CSM=sparse(CSMIJV(:,1),CSMIJV(:,2),CSMIJV(:,3),"<<
    CSM.rows()<<","<<CSM.cols()<<");"<<endl;
#endif

  //printf("CSM_M()\n");
  // Precompute CSM times M for each dimension
  data.CSM_M.resize(data.dim);
#ifdef EXTREME_VERBOSE
  cout<<"data.CSM_M = cell("<<data.dim<<",1);"<<endl;
#endif
  // span of integers from 0 to n-1
  Eigen::Matrix<int,Eigen::Dynamic,1> span_n(n);
  for(int i = 0;i<n;i++)
  {
    span_n(i) = i;
  }

  // span of integers from 0 to M.cols()-1
  Eigen::Matrix<int,Eigen::Dynamic,1> span_mlbs_cols(M.cols());
  for(int i = 0;i<M.cols();i++)
  {
    span_mlbs_cols(i) = i;
  }

  // number of groups
  int k = CSM.rows()/data.dim;
  for(int i = 0;i<data.dim;i++)
  {
    //printf("CSM_M(): Mi\n");
    LbsMatrixType M_i;
    //printf("CSM_M(): slice\n");
    slice(M,(span_n.array()+i*n).matrix().eval(),span_mlbs_cols,M_i);
    LbsMatrixType M_i_dim;
    data.CSM_M[i].resize(k*data.dim,data.m*data.dim*(data.dim+1));
    assert(data.CSM_M[i].cols() == M.cols());
    for(int j = 0;j<data.dim;j++)
    {
      SparseMatrix<double> CSMj;
      //printf("CSM_M(): slice\n");
      slice(
        CSM,
        colon<int>(j*k,(j+1)*k-1),
        colon<int>(j*n,(j+1)*n-1),
        CSMj);
      assert(CSMj.rows() == k);
      assert(CSMj.cols() == n);
      LbsMatrixType CSMjM_i = CSMj * M_i;
      if(is_sparse(CSMjM_i))
      {
        // Convert to full
        //printf("CSM_M(): full\n");
        MatrixXd CSMjM_ifull(CSMjM_i);
//        printf("CSM_M[%d]: %d %d\n",i,data.CSM_M[i].rows(),data.CSM_M[i].cols());
//        printf("CSM_M[%d].block(%d*%d=%d,0,%d,%d): %d %d\n",i,j,k,CSMjM_i.rows(),CSMjM_i.cols(),
//            data.CSM_M[i].block(j*k,0,CSMjM_i.rows(),CSMjM_i.cols()).rows(),
//            data.CSM_M[i].block(j*k,0,CSMjM_i.rows(),CSMjM_i.cols()).cols());
//        printf("CSM_MjMi: %d %d\n",i,CSMjM_i.rows(),CSMjM_i.cols());
//        printf("CSM_MjM_ifull: %d %d\n",i,CSMjM_ifull.rows(),CSMjM_ifull.cols());
        data.CSM_M[i].block(j*k,0,CSMjM_i.rows(),CSMjM_i.cols()) = CSMjM_ifull;
      }else
      {
        data.CSM_M[i].block(j*k,0,CSMjM_i.rows(),CSMjM_i.cols()) = CSMjM_i;
      }
    }
#ifdef EXTREME_VERBOSE
    cout<<"CSM_Mi=["<<endl<<data.CSM_M[i]<<endl<<"];"<<endl;
#endif
  }

  // precompute arap_rhs matrix
  //printf("arap_rhs()\n");
  SparseMatrix<double> K;
  arap_rhs(V,F,V.cols(),data.energy,K);
//#ifdef EXTREME_VERBOSE
//  cout<<"KIJV=["<<endl;print_ijv(K,1);cout<<endl<<"];"<<
//    endl<<"K=sparse(KIJV(:,1),KIJV(:,2),KIJV(:,3),"<<
//    K.rows()<<","<<K.cols()<<");"<<endl;
//#endif
  // Precompute left muliplication by M and right multiplication by G_sum
  SparseMatrix<double> G_sumT = G_sum.transpose();
  SparseMatrix<double> G_sumT_dim_dim;
  repdiag(G_sumT,data.dim*data.dim,G_sumT_dim_dim);
  LbsMatrixType MT = M.transpose();
  // If this is a bottle neck then consider reordering matrix multiplication
  data.M_KG = -4.0 * (MT * (K * G_sumT_dim_dim));
//#ifdef EXTREME_VERBOSE
//  cout<<"data.M_KGIJV=["<<endl;print_ijv(data.M_KG,1);cout<<endl<<"];"<<
//    endl<<"data.M_KG=sparse(data.M_KGIJV(:,1),data.M_KGIJV(:,2),data.M_KGIJV(:,3),"<<
//    data.M_KG.rows()<<","<<data.M_KG.cols()<<");"<<endl;
//#endif

  // Precompute system matrix
  //printf("A()\n");
  SparseMatrix<double> A;
  repdiag(Lapl,data.dim,A);
  data.Q = MT * (A * M);
//#ifdef EXTREME_VERBOSE
//  cout<<"QIJV=["<<endl;print_ijv(data.Q,1);cout<<endl<<"];"<<
//    endl<<"Q=sparse(QIJV(:,1),QIJV(:,2),QIJV(:,3),"<<
//    data.Q.rows()<<","<<data.Q.cols()<<");"<<endl;
//#endif

  // Always do dynamics precomputation so we can hot-switch
  //if(data.with_dynamics)
  //{
    // Build cotangent laplacian
    SparseMatrix<double> Mass;
    //printf("massmatrix()\n");
    massmatrix(V,F,(F.cols()>3?MASSMATRIX_TYPE_BARYCENTRIC:MASSMATRIX_TYPE_VORONOI),Mass);
    //cout<<"MIJV=["<<endl;print_ijv(Mass,1);cout<<endl<<"];"<<
    //  endl<<"M=sparse(MIJV(:,1),MIJV(:,2),MIJV(:,3),"<<
    //  Mass.rows()<<","<<Mass.cols()<<");"<<endl;
    //speye(data.n,Mass);
    SparseMatrix<double> Mass_rep;
    repdiag(Mass,data.dim,Mass_rep);

    // Multiply either side by weights matrix (should be dense)
    data.Mass_tilde = MT * Mass_rep * M;
    MatrixXd ones(data.dim*data.n,data.dim);
    for(int i = 0;i<data.n;i++)
    {
      for(int d = 0;d<data.dim;d++)
      {
        ones(i+d*data.n,d) = 1;
      }
    }
    data.fgrav = MT * (Mass_rep * ones);
    data.fext = MatrixXS::Zero(MT.rows(),1);
    //data.fgrav = MT * (ones);
  //}


  // This may/should be superfluous
  //printf("is_symmetric()\n");
  if(!is_symmetric(data.Q))
  {
    //printf("Fixing symmetry...\n");
    // "Fix" symmetry
    LbsMatrixType QT = data.Q.transpose();
    LbsMatrixType Q_copy = data.Q;
    data.Q = 0.5*(Q_copy+QT);
    // Check that ^^^ this really worked. It doesn't always
    //assert(is_symmetric(*Q));
  }

  //printf("arap_dof_precomputation() succeeded... so far...\n");
  verbose("Number of handles: %i\n", data.m);
  return true;
}

/////////////////////////////////////////////////////////////////////////
//
// STATIC FUNCTIONS (These should be removed or properly defined)
//
/////////////////////////////////////////////////////////////////////////
namespace igl
{
  // returns maximal difference of 'blok' from scalar times 3x3 identity:
  template <typename SSCALAR>
  inline static SSCALAR maxBlokErr(const Eigen::Matrix3f &blok)
  {
    SSCALAR mD;
    SSCALAR value = blok(0,0);
    SSCALAR diff1 = fabs(blok(1,1) - value);
    SSCALAR diff2 = fabs(blok(2,2) - value);
    if (diff1 > diff2) mD = diff1;
    else mD = diff2;
    
    for (int v=0; v<3; v++)
    {
      for (int w=0; w<3; w++)
      {
        if (v == w)
        {
          continue;
        }
        if (mD < fabs(blok(v, w)))
        {
          mD = fabs(blok(v, w));
        }
      }
    }
    
    return mD;
  }
  
  // converts CSM_M_SSCALAR[0], CSM_M_SSCALAR[1], CSM_M_SSCALAR[2] into one
  // "condensed" matrix CSM while checking we're not losing any information by
  // this process; specifically, returns maximal difference from scaled 3x3
  // identity blocks, which should be pretty small number
  template <typename MatrixXS>
  static typename MatrixXS::Scalar condense_CSM(
    const std::vector<MatrixXS> &CSM_M_SSCALAR, 
    int numBones, 
    int dim, 
    MatrixXS &CSM)
  {
    const int numRows = CSM_M_SSCALAR[0].rows();
    assert(CSM_M_SSCALAR[0].cols() == dim*(dim+1)*numBones);
    assert(CSM_M_SSCALAR[1].cols() == dim*(dim+1)*numBones);
    assert(CSM_M_SSCALAR[2].cols() == dim*(dim+1)*numBones);
    assert(CSM_M_SSCALAR[1].rows() == numRows);
    assert(CSM_M_SSCALAR[2].rows() == numRows);
  
    const int numCols = (dim + 1)*numBones;
    CSM.resize(numRows, numCols);
  
    typedef typename MatrixXS::Scalar SSCALAR;
    SSCALAR maxDiff = 0.0f;
  
    for (int r=0; r<numRows; r++)
    {
      for (int coord=0; coord<dim+1; coord++)
      {
        for (int b=0; b<numBones; b++)
        {
          // this is just a test if we really have a multiple of 3x3 identity
          Eigen::Matrix3f blok;
          for (int v=0; v<3; v++)
          {
            for (int w=0; w<3; w++)
            {
              blok(v,w) = CSM_M_SSCALAR[v](r, coord*(numBones*dim) + b + w*numBones);
            }          
          }
  
          //SSCALAR value[3];
          //for (int v=0; v<3; v++)
          //  CSM_M_SSCALAR[v](r, coord*(numBones*dim) + b + v*numBones);
  
          SSCALAR mD = maxBlokErr<SSCALAR>(blok);
          if (mD > maxDiff) maxDiff = mD;
  
          // use the first value:
          CSM(r, coord*numBones + b) = blok(0,0);
        }
      }
    }
  
    return maxDiff;
  }
  
  // splits x_0, ... , x_dim coordinates in column vector 'L' into a numBones*(dimp1) x dim matrix 'Lsep';
  // assumes 'Lsep' has already been preallocated
  //
  // is this the same as uncolumnize? no.
  template <typename MatL, typename MatLsep>
  static void splitColumns(
   const MatL &L, 
   int numBones, 
   int dim, 
   int dimp1, 
   MatLsep &Lsep)
  {
    assert(L.cols() == 1);
    assert(L.rows() == dim*(dimp1)*numBones);
  
    assert(Lsep.rows() == (dimp1)*numBones && Lsep.cols() == dim);
  
    for (int b=0; b<numBones; b++)
    {
      for (int coord=0; coord<dimp1; coord++)
      {
        for (int c=0; c<dim; c++)
        {
          Lsep(coord*numBones + b, c) = L(coord*numBones*dim + c*numBones + b, 0);
        }
      }
    }
  }
  
  
  // the inverse of splitColumns, i.e., takes numBones*(dimp1) x dim matrix 'Lsep' and merges the dimensions
  // into columns vector 'L' (which is assumed to be already allocated):
  //
  // is this the same as columnize? no.
  template <typename MatrixXS>
  static void mergeColumns(const MatrixXS &Lsep, int numBones, int dim, int dimp1, MatrixXS &L)
  {
    assert(L.cols() == 1);
    assert(L.rows() == dim*(dimp1)*numBones);
  
    assert(Lsep.rows() == (dimp1)*numBones && Lsep.cols() == dim);
  
    for (int b=0; b<numBones; b++)
    {
      for (int coord=0; coord<dimp1; coord++)
      {
        for (int c=0; c<dim; c++)
        {
          L(coord*numBones*dim + c*numBones + b, 0) = Lsep(coord*numBones + b, c);
        }
      }
    }
  }
  
  // converts "Solve1" the "rotations" part of FullSolve matrix (the first part)
  // into one "condensed" matrix CSolve1 while checking we're not losing any
  // information by this process; specifically, returns maximal difference from
  // scaled 3x3 identity blocks, which should be pretty small number
  template <typename MatrixXS>
  static typename MatrixXS::Scalar condense_Solve1(MatrixXS &Solve1, int numBones, int numGroups, int dim, MatrixXS &CSolve1)
  {
    assert(Solve1.rows() == dim*(dim + 1)*numBones);
    assert(Solve1.cols() == dim*dim*numGroups);
  
    typedef typename MatrixXS::Scalar SSCALAR;
    SSCALAR maxDiff = 0.0f;
  
    CSolve1.resize((dim + 1)*numBones, dim*numGroups);  
    for (int rowCoord=0; rowCoord<dim+1; rowCoord++)
    {
      for (int b=0; b<numBones; b++)
      {
        for (int colCoord=0; colCoord<dim; colCoord++)
        {
          for (int g=0; g<numGroups; g++)
          {
            Eigen::Matrix3f blok;
            for (int r=0; r<3; r++)
            {
              for (int c=0; c<3; c++)
              {
                blok(r, c) = Solve1(rowCoord*numBones*dim + r*numBones + b, colCoord*numGroups*dim + c*numGroups + g);
              }
            }
  
            SSCALAR mD = maxBlokErr<SSCALAR>(blok);
            if (mD > maxDiff) maxDiff = mD;
  
            CSolve1(rowCoord*numBones + b, colCoord*numGroups + g) = blok(0,0);
          }
        }
      }
    }  
    
    return maxDiff;
  }
}

template <typename LbsMatrixType, typename SSCALAR>
IGL_INLINE bool igl::arap_dof_recomputation(
  const Eigen::Matrix<int,Eigen::Dynamic,1> & fixed_dim,
  const Eigen::SparseMatrix<double> & A_eq,
  ArapDOFData<LbsMatrixType, SSCALAR> & data)
{
  using namespace Eigen;
  typedef Matrix<SSCALAR, Dynamic, Dynamic> MatrixXS;

  LbsMatrixType * Q;
  LbsMatrixType Qdyn;
  if(data.with_dynamics)
  {
    // multiply by 1/timestep and to quadratic coefficients matrix
    // Might be missing a 0.5 here
    LbsMatrixType Q_copy = data.Q;
    Qdyn = Q_copy + (1.0/(data.h*data.h))*data.Mass_tilde;
    Q = &Qdyn;

    // This may/should be superfluous
    //printf("is_symmetric()\n");
    if(!is_symmetric(*Q))
    {
      //printf("Fixing symmetry...\n");
      // "Fix" symmetry
      LbsMatrixType QT = (*Q).transpose();
      LbsMatrixType Q_copy = *Q;
      *Q = 0.5*(Q_copy+QT);
      // Check that ^^^ this really worked. It doesn't always
      //assert(is_symmetric(*Q));
    }
  }else
  {
    Q = &data.Q;
  }

  assert((int)data.CSM_M.size() == data.dim);
  assert(A_eq.cols() == data.m*data.dim*(data.dim+1));
  data.fixed_dim = fixed_dim;

  if(fixed_dim.size() > 0)
  {
    assert(fixed_dim.maxCoeff() < data.m*data.dim*(data.dim+1));
    assert(fixed_dim.minCoeff() >= 0);
  }

#ifdef EXTREME_VERBOSE
  cout<<"data.fixed_dim=["<<endl<<data.fixed_dim<<endl<<"]+1;"<<endl;
#endif

  // Compute dense solve matrix (alternative of matrix factorization)
  //printf("min_quad_dense_precompute()\n");
  MatrixXd Qfull(*Q);
  MatrixXd A_eqfull(A_eq);
  MatrixXd M_Solve;

  double timer0_start = get_seconds_hires();
  bool use_lu = data.effective_dim != 2;
  //use_lu = false;
  //printf("use_lu: %s\n",(use_lu?"TRUE":"FALSE"));
  min_quad_dense_precompute(Qfull, A_eqfull, use_lu,M_Solve);
  double timer0_end = get_seconds_hires();
  verbose("Bob timing: %.20f\n", (timer0_end - timer0_start)*1000.0);

  // Precompute full solve matrix:
  const int fsRows = data.m * data.dim * (data.dim + 1); // 12 * number_of_bones
  const int fsCols1 = data.M_KG.cols(); // 9 * number_of_posConstraints
  const int fsCols2 = A_eq.rows(); // number_of_posConstraints
  data.M_FullSolve.resize(fsRows, fsCols1 + fsCols2);
  // note the magical multiplicative constant "-0.5", I've no idea why it has
  // to be there :)
  data.M_FullSolve << 
    (-0.5 * M_Solve.block(0, 0, fsRows, fsRows) * data.M_KG).template cast<SSCALAR>(), 
    M_Solve.block(0, fsRows, fsRows, fsCols2).template cast<SSCALAR>();

  if(data.with_dynamics)
  {
    printf(
      "---------------------------------------------------------------------\n"
      "\n\n\nWITH DYNAMICS recomputation\n\n\n"
      "---------------------------------------------------------------------\n"
      );
    // Also need to save Π1 before it gets multiplied by Ktilde (aka M_KG)
    data.Pi_1 = M_Solve.block(0, 0, fsRows, fsRows).template cast<SSCALAR>();
  }

  // Precompute condensed matrices,
  // first CSM:
  std::vector<MatrixXS> CSM_M_SSCALAR;
  CSM_M_SSCALAR.resize(data.dim);
  for (int i=0; i<data.dim; i++) CSM_M_SSCALAR[i] = data.CSM_M[i].template cast<SSCALAR>();
  SSCALAR maxErr1 = condense_CSM(CSM_M_SSCALAR, data.m, data.dim, data.CSM);  
  verbose("condense_CSM maxErr = %.15f (this should be close to zero)\n", maxErr1);
  assert(fabs(maxErr1) < 1e-5);
  
  // and then solveBlock1:
  // number of groups
  const int k = data.CSM_M[0].rows()/data.dim;
  MatrixXS SolveBlock1 = data.M_FullSolve.block(0, 0, data.M_FullSolve.rows(), data.dim * data.dim * k);
  SSCALAR maxErr2 = condense_Solve1(SolveBlock1, data.m, k, data.dim, data.CSolveBlock1);  
  verbose("condense_Solve1 maxErr = %.15f (this should be close to zero)\n", maxErr2);
  assert(fabs(maxErr2) < 1e-5);

  return true;
}

template <typename LbsMatrixType, typename SSCALAR>
IGL_INLINE bool igl::arap_dof_update(
  const ArapDOFData<LbsMatrixType, SSCALAR> & data,
  const Eigen::Matrix<double,Eigen::Dynamic,1> & B_eq,
  const Eigen::MatrixXd & L0,
  const int max_iters,
  const double 
#ifdef IGL_ARAP_DOF_FIXED_ITERATIONS_COUNT
  tol,
#else
  /*tol*/,
#endif
  Eigen::MatrixXd & L
  )
{
  using namespace Eigen;
  typedef Matrix<SSCALAR, Dynamic, Dynamic> MatrixXS;
#ifdef ARAP_GLOBAL_TIMING
  double timer_start = get_seconds_hires();
#endif

  // number of dimensions
  assert((int)data.CSM_M.size() == data.dim);
  assert((int)L0.size() == (data.m)*data.dim*(data.dim+1));
  assert(max_iters >= 0);
  assert(tol >= 0);

  // timing variables
  double 
    sec_start, 
    sec_covGather, 
    sec_fitRotations, 
    //sec_rhs, 
    sec_prepMult, 
    sec_solve, sec_end;

  assert(L0.cols() == 1);
#ifdef EXTREME_VERBOSE
  cout<<"dim="<<data.dim<<";"<<endl;
  cout<<"m="<<data.m<<";"<<endl;
#endif

  // number of groups
  const int k = data.CSM_M[0].rows()/data.dim;
  for(int i = 0;i<data.dim;i++)
  {
    assert(data.CSM_M[i].rows()/data.dim == k);
  }
#ifdef EXTREME_VERBOSE
  cout<<"k="<<k<<";"<<endl;
#endif

  // resize output and initialize with initial guess
  L = L0;
#ifndef IGL_ARAP_DOF_FIXED_ITERATIONS_COUNT
  // Keep track of last solution
  MatrixXS L_prev;
#endif
  // We will be iterating on L_SSCALAR, only at the end we convert back to double
  MatrixXS L_SSCALAR = L.cast<SSCALAR>();

  int iters = 0;
#ifndef IGL_ARAP_DOF_FIXED_ITERATIONS_COUNT
  double max_diff = tol+1;  
#endif

  MatrixXS S(k*data.dim,data.dim);
  MatrixXS R(data.dim,data.dim*k);
  Eigen::Matrix<SSCALAR,Eigen::Dynamic,1> Rcol(data.dim * data.dim * k);
  Matrix<SSCALAR,Dynamic,1> B_eq_SSCALAR = B_eq.cast<SSCALAR>();
  Matrix<SSCALAR,Dynamic,1> B_eq_fix_SSCALAR;
  Matrix<SSCALAR,Dynamic,1> L0SSCALAR = L0.cast<SSCALAR>();
  slice(L0SSCALAR, data.fixed_dim, B_eq_fix_SSCALAR);    
  //MatrixXS rhsFull(Rcol.rows() + B_eq.rows() + B_eq_fix_SSCALAR.rows(), 1); 

  MatrixXS Lsep(data.m*(data.dim + 1), 3);  
  const MatrixXS L_part2 = 
    data.M_FullSolve.block(0, Rcol.rows(), data.M_FullSolve.rows(), B_eq_SSCALAR.rows()) * B_eq_SSCALAR;
  const MatrixXS L_part3 = 
    data.M_FullSolve.block(0, Rcol.rows() + B_eq_SSCALAR.rows(), data.M_FullSolve.rows(), B_eq_fix_SSCALAR.rows()) * B_eq_fix_SSCALAR;
  MatrixXS L_part2and3 = L_part2 + L_part3;

  // preallocate workspace variables:
  MatrixXS Rxyz(k*data.dim, data.dim);  
  MatrixXS L_part1xyz((data.dim + 1) * data.m, data.dim);
  MatrixXS L_part1(data.dim * (data.dim + 1) * data.m, 1);

#ifdef ARAP_GLOBAL_TIMING
    double timer_prepFinished = get_seconds_hires();
#endif

#ifdef IGL_ARAP_DOF_FIXED_ITERATIONS_COUNT
  while(iters < max_iters)
#else
  while(iters < max_iters && max_diff > tol)
#endif
  {  
    if(data.print_timings)
    {
      sec_start = get_seconds_hires();
    }

#ifndef IGL_ARAP_DOF_FIXED_ITERATIONS_COUNT
    L_prev = L_SSCALAR;
#endif
    ///////////////////////////////////////////////////////////////////////////
    // Local step: Fix positions, fit rotations
    ///////////////////////////////////////////////////////////////////////////    
  
    // Gather covariance matrices    

    splitColumns(L_SSCALAR, data.m, data.dim, data.dim + 1, Lsep);

    S = data.CSM * Lsep; 
    // interestingly, this doesn't seem to be so slow, but
    //MKL is still 2x faster (probably due to AVX)
    //#ifdef IGL_ARAP_DOF_DOUBLE_PRECISION_SOLVE
    //    MKL_matMatMult_double(S, data.CSM, Lsep);
    //#else
    //    MKL_matMatMult_single(S, data.CSM, Lsep);
    //#endif
    
    if(data.print_timings)
    {
      sec_covGather = get_seconds_hires();
    }

#ifdef EXTREME_VERBOSE
    cout<<"S=["<<endl<<S<<endl<<"];"<<endl;
#endif
    // Fit rotations to covariance matrices
    if(data.effective_dim == 2)
    {
      fit_rotations_planar(S,R);
    }else
    {
#ifdef __SSE__ // fit_rotations_SSE will convert to float if necessary
      fit_rotations_SSE(S,R);
#else
      fit_rotations(S,false,R);
#endif
    }

#ifdef EXTREME_VERBOSE
    cout<<"R=["<<endl<<R<<endl<<"];"<<endl;
#endif  

    if(data.print_timings)
    {
      sec_fitRotations = get_seconds_hires();
    }
  
    ///////////////////////////////////////////////////////////////////////////
    // "Global" step: fix rotations per mesh vertex, solve for
    // linear transformations at handles
    ///////////////////////////////////////////////////////////////////////////

    // all this shuffling is retarded and not completely negligible time-wise;
    // TODO: change fit_rotations_XXX so it returns R in the format ready for
    // CSolveBlock1 multiplication
    columnize(R, k, 2, Rcol);
#ifdef EXTREME_VERBOSE
    cout<<"Rcol=["<<endl<<Rcol<<endl<<"];"<<endl;
#endif  
    splitColumns(Rcol, k, data.dim, data.dim, Rxyz);
    
    if(data.print_timings)
    {
      sec_prepMult = get_seconds_hires();
    }  
    
    L_part1xyz = data.CSolveBlock1 * Rxyz;
    //#ifdef IGL_ARAP_DOF_DOUBLE_PRECISION_SOLVE
    //    MKL_matMatMult_double(L_part1xyz, data.CSolveBlock1, Rxyz);    
    //#else
    //    MKL_matMatMult_single(L_part1xyz, data.CSolveBlock1, Rxyz);    
    //#endif
    mergeColumns(L_part1xyz, data.m, data.dim, data.dim + 1, L_part1);

    if(data.with_dynamics)
    {
      // Consider reordering or precomputing matrix multiplications
      MatrixXS L_part1_dyn(data.dim * (data.dim + 1) * data.m, 1);
      // Eigen can't parse this:
      //L_part1_dyn = 
      //  -(2.0/(data.h*data.h)) * data.Pi_1 * data.Mass_tilde * data.L0 +
      //   (1.0/(data.h*data.h)) * data.Pi_1 * data.Mass_tilde * data.Lm1;
      // -1.0 because we've moved these linear terms to the right hand side
      //MatrixXS temp = -1.0 * 
      //    ((-2.0/(data.h*data.h)) * data.L0.array() + 
      //      (1.0/(data.h*data.h)) * data.Lm1.array()).matrix();
      //MatrixXS temp = -1.0 * 
      //    ( (-1.0/(data.h*data.h)) * data.L0.array() + 
      //      (1.0/(data.h*data.h)) * data.Lm1.array()
      //      (-1.0/(data.h*data.h)) * data.L0.array() + 
      //      ).matrix();
      //Lvel0 = (1.0/(data.h)) * data.Lm1.array() - data.L0.array();
      MatrixXS temp = -1.0 * 
          ( (-1.0/(data.h*data.h)) * data.L0.array() + 
            (1.0/(data.h)) * data.Lvel0.array()
            ).matrix();
      MatrixXd temp_d = temp.template cast<double>();

      MatrixXd temp_g = data.fgrav*(data.grav_mag*data.grav_dir);

      assert(data.fext.rows() == temp_g.rows());
      assert(data.fext.cols() == temp_g.cols());
      MatrixXd temp2 = data.Mass_tilde * temp_d + temp_g + data.fext.template cast<double>();
      MatrixXS temp2_f = temp2.template cast<SSCALAR>();
      L_part1_dyn = data.Pi_1 * temp2_f;
      L_part1.array() = L_part1.array() + L_part1_dyn.array();
    }

    //L_SSCALAR = L_part1 + L_part2and3;
    assert(L_SSCALAR.rows() == L_part1.rows() && L_SSCALAR.rows() == L_part2and3.rows());
    for (int i=0; i<L_SSCALAR.rows(); i++)
    {
      L_SSCALAR(i, 0) = L_part1(i, 0) + L_part2and3(i, 0);
    }

#ifdef EXTREME_VERBOSE
    cout<<"L=["<<endl<<L<<endl<<"];"<<endl;
#endif  

    if(data.print_timings)
    {
      sec_solve = get_seconds_hires();
    }

#ifndef IGL_ARAP_DOF_FIXED_ITERATIONS_COUNT
    // Compute maximum absolute difference with last iteration's solution
    max_diff = (L_SSCALAR-L_prev).eval().array().abs().matrix().maxCoeff();
#endif
    iters++;  

    if(data.print_timings)
    {
      sec_end = get_seconds_hires();
#ifndef WIN32
      // trick to get sec_* variables to compile without warning on mac
      if(false)
#endif
      printf(
        "\ntotal iteration time = %f "
        "[local: covGather = %f, "
        "fitRotations = %f, "
        "global: prep = %f, "
        "solve = %f, "
        "error = %f [ms]]\n", 
        (sec_end - sec_start)*1000.0, 
        (sec_covGather - sec_start)*1000.0, 
        (sec_fitRotations - sec_covGather)*1000.0, 
        (sec_prepMult - sec_fitRotations)*1000.0, 
        (sec_solve - sec_prepMult)*1000.0, 
        (sec_end - sec_solve)*1000.0 );
    }
  }


  L = L_SSCALAR.template cast<double>();
  assert(L.cols() == 1);

#ifdef ARAP_GLOBAL_TIMING
  double timer_finito = get_seconds_hires();
  printf(
    "ARAP preparation = %f, "
    "all %i iterations = %f [ms]\n", 
    (timer_prepFinished - timer_start)*1000.0, 
    max_iters, 
    (timer_finito - timer_prepFinished)*1000.0);  
#endif

  return true;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::arap_dof_update<Eigen::Matrix<double, -1, -1, 0, -1, -1>, double>(ArapDOFData<Eigen::Matrix<double, -1, -1, 0, -1, -1>, double> const&, Eigen::Matrix<double, -1, 1, 0, -1, 1> const&, Eigen::Matrix<double, -1, -1, 0, -1, -1> const&, int, double, Eigen::Matrix<double, -1, -1, 0, -1, -1>&);
template bool igl::arap_dof_recomputation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, double>(Eigen::Matrix<int, -1, 1, 0, -1, 1> const&, Eigen::SparseMatrix<double, 0, int> const&, ArapDOFData<Eigen::Matrix<double, -1, -1, 0, -1, -1>, double>&);
template bool igl::arap_dof_precomputation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, double>(Eigen::Matrix<double, -1, -1, 0, -1, -1> const&, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&, Eigen::Matrix<double, -1, -1, 0, -1, -1> const&, Eigen::Matrix<int, -1, 1, 0, -1, 1> const&, ArapDOFData<Eigen::Matrix<double, -1, -1, 0, -1, -1>, double>&);
template bool igl::arap_dof_update<Eigen::Matrix<double, -1, -1, 0, -1, -1>, float>(igl::ArapDOFData<Eigen::Matrix<double, -1, -1, 0, -1, -1>, float> const&, Eigen::Matrix<double, -1, 1, 0, -1, 1> const&, Eigen::Matrix<double, -1, -1, 0, -1, -1> const&, int, double, Eigen::Matrix<double, -1, -1, 0, -1, -1>&);
template bool igl::arap_dof_recomputation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, float>(Eigen::Matrix<int, -1, 1, 0, -1, 1> const&, Eigen::SparseMatrix<double, 0, int> const&, igl::ArapDOFData<Eigen::Matrix<double, -1, -1, 0, -1, -1>, float>&);
template bool igl::arap_dof_precomputation<Eigen::Matrix<double, -1, -1, 0, -1, -1>, float>(Eigen::Matrix<double, -1, -1, 0, -1, -1> const&, Eigen::Matrix<int, -1, -1, 0, -1, -1> const&, Eigen::Matrix<double, -1, -1, 0, -1, -1> const&, Eigen::Matrix<int, -1, 1, 0, -1, 1> const&, igl::ArapDOFData<Eigen::Matrix<double, -1, -1, 0, -1, -1>, float>&);
#endif
