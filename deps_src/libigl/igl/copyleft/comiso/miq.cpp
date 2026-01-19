// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>, Olga Diamanti <olga.diam@gmail.com>, Kevin Walliman <wkevin@student.ethz.ch>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "miq.h"
#include "../../local_basis.h"
#include "../../triangle_triangle_adjacency.h"
#include "../../cut_mesh.h"
#include "../../LinSpaced.h"

// includes for VertexIndexing
#include "../../HalfEdgeIterator.h"
#include "../../is_border_vertex.h"
#include "../../vertex_triangle_adjacency.h"

// includes for PoissonSolver
#include "../../slice_into.h"
#include "../../grad.h"
#include "../../cotmatrix.h"
#include "../../doublearea.h"
#include <gmm/gmm.h>
#include <CoMISo/Solver/ConstrainedSolver.hh>
#include <CoMISo/Solver/MISolver.hh>
#include <CoMISo/Solver/GMM_Tools.hh>

//
#include "../../cross_field_missmatch.h"
#include "../../comb_frame_field.h"
#include "../../comb_cross_field.h"
#include "../../cut_mesh_from_singularities.h"
#include "../../find_cross_field_singularities.h"
#include "../../compute_frame_field_bisectors.h"
#include "../../rotate_vectors.h"

#ifndef NDEBUG
#include <fstream>
#endif
#include <iostream>
#include "../../matlab_format.h"

#define DEBUGPRINT 0


namespace igl {
namespace copyleft {
namespace comiso {
  struct SeamInfo
  {
    int v0,v0p;
    int integerVar;
    unsigned char MMatch;

    IGL_INLINE SeamInfo(int _v0,
                        int _v0p,
                        int _MMatch,
                        int _integerVar);

    IGL_INLINE SeamInfo(const SeamInfo &S1);
  };

  struct MeshSystemInfo
  {
    ////number of vertices variables
    int num_vert_variables;
    ///num of integer for cuts
    int num_integer_cuts;
    ///this are used for drawing purposes
    std::vector<SeamInfo> EdgeSeamInfo;
  };


  template <typename DerivedV, typename DerivedF>
  class VertexIndexing
  {
  public:
    // Input:
    const Eigen::PlainObjectBase<DerivedV> &V;
    const Eigen::PlainObjectBase<DerivedF> &F;
    const Eigen::PlainObjectBase<DerivedV> &Vcut;
    const Eigen::PlainObjectBase<DerivedF> &Fcut;
    const Eigen::PlainObjectBase<DerivedF> &TT;
    const Eigen::PlainObjectBase<DerivedF> &TTi;

    const Eigen::Matrix<int, Eigen::Dynamic, 3> &Handle_MMatch;
    const Eigen::Matrix<int, Eigen::Dynamic, 1> &Handle_Singular; // bool
    const Eigen::Matrix<int, Eigen::Dynamic, 3> &Handle_Seams; // 3 bool


    ///this handle for mesh TODO: move with the other global variables
    MeshSystemInfo Handle_SystemInfo;

    IGL_INLINE VertexIndexing(const Eigen::PlainObjectBase<DerivedV> &_V,
                              const Eigen::PlainObjectBase<DerivedF> &_F,
                              const Eigen::PlainObjectBase<DerivedV> &_Vcut,
                              const Eigen::PlainObjectBase<DerivedF> &_Fcut,
                              const Eigen::PlainObjectBase<DerivedF> &_TT,
                              const Eigen::PlainObjectBase<DerivedF> &_TTi,
                              const Eigen::Matrix<int, Eigen::Dynamic, 3> &_Handle_MMatch,
                              const Eigen::Matrix<int, Eigen::Dynamic, 1> &_Handle_Singular,
                              const Eigen::Matrix<int, Eigen::Dynamic, 3> &_Handle_Seams
                              );

    // provide information about every vertex per seam
    IGL_INLINE void InitSeamInfo();


  private:
    struct VertexInfo{
      int v;  // vertex index (according to V)
      int f0, k0; // face and local edge information of the edge that connects this vertex to the previous vertex (previous in the vector)
      int f1, k1; // face and local edge information of the other face corresponding to the same edge
      VertexInfo(int _v, int _f0, int _k0, int _f1, int _k1) :
                 v(_v), f0(_f0), k0(_k0), f1(_f1), k1(_k1){}
      bool operator==(VertexInfo const& other){
        return other.v == v;
      }
    };

    IGL_INLINE void GetSeamInfo(const int f0,
                                const int f1,
                                const int indexE,
                                int &v0,int &v1,
                                int &v0p,int &v1p,
                                unsigned char &_MMatch);

    IGL_INLINE std::vector<std::vector<VertexInfo> > GetVerticesPerSeam();
  };


  template <typename DerivedV, typename DerivedF>
  class PoissonSolver
  {

  public:
    IGL_INLINE void SolvePoisson(Eigen::VectorXd Stiffness,
                                 double vector_field_scale=0.1f,
                                 double grid_res=1.f,
                                 bool direct_round=true,
                                 int localIter=0,
                                 bool _integer_rounding=true,
                                 bool _singularity_rounding=true,
                                 std::vector<int> roundVertices = std::vector<int>(),
                                 std::vector<std::vector<int> > hardFeatures = std::vector<std::vector<int> >());

    IGL_INLINE PoissonSolver(const Eigen::PlainObjectBase<DerivedV> &_V,
                             const Eigen::PlainObjectBase<DerivedF> &_F,
                             const Eigen::PlainObjectBase<DerivedV> &_Vcut,
                             const Eigen::PlainObjectBase<DerivedF> &_Fcut,
                             const Eigen::PlainObjectBase<DerivedF> &_TT,
                             const Eigen::PlainObjectBase<DerivedF> &_TTi,
                             const Eigen::PlainObjectBase<DerivedV> &_PD1,
                             const Eigen::PlainObjectBase<DerivedV> &_PD2,
                             const Eigen::Matrix<int, Eigen::Dynamic, 1>&_Handle_Singular,
                             const MeshSystemInfo &_Handle_SystemInfo
                             );

    const Eigen::PlainObjectBase<DerivedV> &V;
    const Eigen::PlainObjectBase<DerivedF> &F;
    const Eigen::PlainObjectBase<DerivedV> &Vcut;
    const Eigen::PlainObjectBase<DerivedF> &Fcut;
    const Eigen::PlainObjectBase<DerivedF> &TT;
    const Eigen::PlainObjectBase<DerivedF> &TTi;
    const Eigen::PlainObjectBase<DerivedV> &PD1;
    const Eigen::PlainObjectBase<DerivedV> &PD2;
    const Eigen::Matrix<int, Eigen::Dynamic, 1> &Handle_Singular; // bool

    const MeshSystemInfo &Handle_SystemInfo;

    // Internal:
    Eigen::VectorXd Handle_Stiffness;
    std::vector<std::vector<int> > VF;
    std::vector<std::vector<int> > VFi;
    Eigen::MatrixXd UV; // this is probably useless

    // Output:
    // per wedge UV coordinates, 6 coordinates (1 face) per row
    Eigen::MatrixXd WUV;
    // per vertex UV coordinates, Vcut.rows() x 2
    Eigen::MatrixXd UV_out;

    // Matrices
    Eigen::SparseMatrix<double> Lhs;
    Eigen::SparseMatrix<double> Constraints;
    Eigen::VectorXd rhs;
    Eigen::VectorXd constraints_rhs;
    ///vector of unknowns
    std::vector< double > X;

    ////REAL PART
    ///number of fixed vertex
    unsigned int n_fixed_vars;

    ///the number of REAL variables for vertices
    unsigned int n_vert_vars;

    ///total number of variables of the system,
    ///do not consider constraints, but consider integer vars
    unsigned int num_total_vars;

    //////INTEGER PART
    ///the total number of integer variables
    unsigned int n_integer_vars;

    ///CONSTRAINT PART
    ///number of cuts constraints
    unsigned int num_cut_constraint;

    // number of user-defined constraints
    unsigned int num_userdefined_constraint;

    ///total number of constraints equations
    unsigned int num_constraint_equations;

    ///vector of blocked vertices
    std::vector<int> Hard_constraints;

    ///vector of indexes to round
    std::vector<int> ids_to_round;

    ///vector of indexes to round
    std::vector<std::vector<int > > userdefined_constraints;

    ///boolean that is true if rounding to integer is needed
    bool integer_rounding;

    ///START COMMON MATH FUNCTIONS
    ///return the complex encoding the rotation
    ///for a given missmatch interval
    IGL_INLINE std::complex<double> GetRotationComplex(int interval);
    ///END COMMON MATH FUNCTIONS

    ///START FIXING VERTICES
    ///set a given vertex as fixed
    IGL_INLINE void AddFixedVertex(int v);

    ///find vertex to fix in case we're using
    ///a vector field NB: multiple components not handled
    IGL_INLINE void FindFixedVertField();

    ///find hard constraint depending if using or not
    ///a vector field
    IGL_INLINE void FindFixedVert();

    IGL_INLINE int GetFirstVertexIndex(int v);

    ///fix the vertices which are flagged as fixed
    IGL_INLINE void FixBlockedVertex();
    ///END FIXING VERTICES

    ///HANDLING SINGULARITY
    //set the singularity round to integer location
    IGL_INLINE void AddSingularityRound();

    IGL_INLINE void AddToRoundVertices(std::vector<int> ids);

    ///START GENERIC SYSTEM FUNCTIONS
    //build the laplacian matrix cyclyng over all rangemaps
    //and over all faces
    IGL_INLINE void BuildLaplacianMatrix(double vfscale=1);

    ///find different sized of the system
    IGL_INLINE void FindSizes();

    IGL_INLINE void AllocateSystem();

    ///intitialize the whole matrix
    IGL_INLINE void InitMatrix();

    ///map back coordinates after that
    ///the system has been solved
    IGL_INLINE void MapCoords();
    ///END GENERIC SYSTEM FUNCTIONS

    ///set the constraints for the inter-range cuts
    IGL_INLINE void BuildSeamConstraintsExplicitTranslation();

    ///set the constraints for the inter-range cuts
    IGL_INLINE void BuildUserDefinedConstraints();

    ///call of the mixed integer solver
    IGL_INLINE void MixedIntegerSolve(double cone_grid_res=1,
                                      bool direct_round=true,
                                      int localIter=0);

    IGL_INLINE void clearUserConstraint();

    IGL_INLINE void addSharpEdgeConstraint(int fid, int vid);

  };

  template <typename DerivedV, typename DerivedF, typename DerivedU>
  class MIQ_class
  {
  private:
    const Eigen::PlainObjectBase<DerivedV> &V;
    const Eigen::PlainObjectBase<DerivedF> &F;
    DerivedV Vcut;
    DerivedF Fcut;
    Eigen::MatrixXd UV_out;
    DerivedF FUV_out;

    // internal
    DerivedF TT;
    DerivedF TTi;

    // Stiffness per face
    Eigen::VectorXd Handle_Stiffness;
    DerivedV B1, B2, B3;

  public:
    IGL_INLINE MIQ_class(const Eigen::PlainObjectBase<DerivedV> &V_,
                         const Eigen::PlainObjectBase<DerivedF> &F_,
                         const Eigen::PlainObjectBase<DerivedV> &PD1_combed,
                         const Eigen::PlainObjectBase<DerivedV> &PD2_combed,
                         const Eigen::Matrix<int, Eigen::Dynamic, 3> &Handle_MMatch,
                         const Eigen::Matrix<int, Eigen::Dynamic, 1> &Handle_Singular,
                         const Eigen::Matrix<int, Eigen::Dynamic, 3> &Handle_Seams,
                         Eigen::PlainObjectBase<DerivedU> &UV,
                         Eigen::PlainObjectBase<DerivedF> &FUV,
                         double GradientSize = 30.0,
                         double Stiffness = 5.0,
                         bool DirectRound = false,
                         int iter = 5,
                         int localIter = 5,
                         bool DoRound = true,
                         bool SingularityRound=true,
                         std::vector<int> roundVertices = std::vector<int>(),
                         std::vector<std::vector<int> > hardFeatures = std::vector<std::vector<int> >());


    IGL_INLINE void extractUV(Eigen::PlainObjectBase<DerivedU> &UV_out,
                              Eigen::PlainObjectBase<DerivedF> &FUV_out);

  private:
    IGL_INLINE int NumFlips(const Eigen::MatrixXd& WUV);

    IGL_INLINE double Distortion(int f, double h, const Eigen::MatrixXd& WUV);

    IGL_INLINE double LaplaceDistortion(const int f, double h, const Eigen::MatrixXd& WUV);

    IGL_INLINE bool updateStiffeningJacobianDistorsion(double grad_size, const Eigen::MatrixXd& WUV);

    IGL_INLINE bool IsFlipped(const Eigen::Vector2d &uv0,
                              const Eigen::Vector2d &uv1,
                              const Eigen::Vector2d &uv2);

    IGL_INLINE bool IsFlipped(const int i, const Eigen::MatrixXd& WUV);

  };
};
};
}

IGL_INLINE igl::copyleft::comiso::SeamInfo::SeamInfo(int _v0,
                                   int _v0p,
                                   int _MMatch,
                                   int _integerVar)
{
  v0=_v0;
  v0p=_v0p;
  integerVar=_integerVar;
  MMatch=_MMatch;
}

IGL_INLINE igl::copyleft::comiso::SeamInfo::SeamInfo(const SeamInfo &S1)
{
  v0=S1.v0;
  v0p=S1.v0p;
  integerVar=S1.integerVar;
  MMatch=S1.MMatch;
}


template <typename DerivedV, typename DerivedF>
IGL_INLINE igl::copyleft::comiso::VertexIndexing<DerivedV, DerivedF>::VertexIndexing(const Eigen::PlainObjectBase<DerivedV> &_V,
                                                                   const Eigen::PlainObjectBase<DerivedF> &_F,
                                                                   const Eigen::PlainObjectBase<DerivedV> &_Vcut,
                                                                   const Eigen::PlainObjectBase<DerivedF> &_Fcut,
                                                                   const Eigen::PlainObjectBase<DerivedF> &_TT,
                                                                   const Eigen::PlainObjectBase<DerivedF> &_TTi,
                                                                   const Eigen::Matrix<int, Eigen::Dynamic, 3> &_Handle_MMatch,
                                                                   const Eigen::Matrix<int, Eigen::Dynamic, 1> &_Handle_Singular,
                                                                   const Eigen::Matrix<int, Eigen::Dynamic, 3> &_Handle_Seams
                                                                   ):
V(_V),
F(_F),
Vcut(_Vcut),
Fcut(_Fcut),
TT(_TT),
TTi(_TTi),
Handle_MMatch(_Handle_MMatch),
Handle_Singular(_Handle_Singular),
Handle_Seams(_Handle_Seams)
{
  #ifdef DEBUG_PRINT
  cerr<<igl::matlab_format(Handle_Seams,"Handle_Seams");
#endif

  Handle_SystemInfo.num_vert_variables=Vcut.rows();
  Handle_SystemInfo.num_integer_cuts=0;
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::VertexIndexing<DerivedV, DerivedF>::GetSeamInfo(const int f0,
                                                                     const int f1,
                                                                     const int indexE,
                                                                     int &v0,int &v1,
                                                                     int &v0p,int &v1p,
                                                                     unsigned char &_MMatch)
{
  int edgef0 = indexE;
  v0 = Fcut(f0,edgef0);
  v1 = Fcut(f0,(edgef0+1)%3);
  ////get the index on opposite side
  assert(TT(f0,edgef0) == f1);
  int edgef1 = TTi(f0,edgef0);
  v1p = Fcut(f1,edgef1);
  v0p = Fcut(f1,(edgef1+1)%3);

  _MMatch = Handle_MMatch(f0,edgef0);
  assert(F(f0,edgef0)         == F(f1,((edgef1+1)%3)));
  assert(F(f0,((edgef0+1)%3)) == F(f1,edgef1));
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE std::vector<std::vector<typename igl::copyleft::comiso::VertexIndexing<DerivedV, DerivedF>::VertexInfo> > igl::copyleft::comiso::VertexIndexing<DerivedV, DerivedF>::GetVerticesPerSeam()
{
  // Return value
  std::vector<std::vector<VertexInfo> >verticesPerSeam;

  // for every vertex, keep track of their adjacent vertices on seams.
  // regular vertices have two neighbors on a seam, start- and endvertices may have any other numbers of neighbors (e.g. 1 or 3)
  std::vector<std::list<VertexInfo> > VVSeam(V.rows());
  Eigen::MatrixXi F_hit = Eigen::MatrixXi::Zero(F.rows(), 3);
  for (unsigned int f=0; f<F.rows();f++)
  {
    int f0 = f;
    for(int k0=0; k0<3; k0++){
      int f1 = TT(f0,k0);
      if(f1 == -1)
        continue;

      bool seam = Handle_Seams(f0,k0);
      if (seam && F_hit(f0,k0) == 0)
      {
        int v0 = F(f0, k0);
        int v1 = F(f0, (k0+1)%3);
        int k1 = TTi(f0,k0);
        VVSeam[v0].push_back(VertexInfo(v1, f0, k0, f1, k1));
        VVSeam[v1].push_back(VertexInfo(v0, f0, k0, f1, k1));
        F_hit(f0, k0) = 1;
        F_hit(f1, k1) = 1;
      }
    }
  }

  // Find start vertices, i.e. vertices that start or end a seam branch
  std::vector<int> startVertexIndices;
  std::vector<bool> isStartVertex(V.rows());
  for (unsigned int i=0;i<V.rows();i++)
  {
    isStartVertex[i] = false;
    // vertices with two neighbors are regular vertices, unless the vertex is a singularity, in which case it qualifies as a start vertex
    if (VVSeam[i].size() > 0 && VVSeam[i].size() != 2 || Handle_Singular(i) == true)
    {
      startVertexIndices.push_back(i);
      isStartVertex[i] = true;
    }
  }

  // For each startVertex, walk along its seam
  for (unsigned int i=0;i<startVertexIndices.size();i++)
  {
    auto startVertexNeighbors = &VVSeam[startVertexIndices[i]];
    const int neighborSize = startVertexNeighbors->size();

    // explore every seam to which this vertex is a start vertex
    // note: a vertex can never be a start vertex and a regular vertex simultaneously
    for (unsigned int j=0;j<neighborSize;j++)
    {
      std::vector<VertexInfo> thisSeam; // temporary container

      // Create vertexInfo struct for start vertex
      auto startVertex = VertexInfo(startVertexIndices[i], -1, -1, -1, -1);// -1 values are arbitrary (will never be used)
      auto currentVertex = startVertex;
      // Add start vertex to the seam
      thisSeam.push_back(currentVertex);

      // advance on the seam
      auto currentVertexNeighbors = startVertexNeighbors;
      auto nextVertex = currentVertexNeighbors->front();
      currentVertexNeighbors->pop_front();

      auto prevVertex = startVertex; // bogus initialization to get the type
      while (true)
      {
        // move to the next vertex
        prevVertex = currentVertex;
        currentVertex = nextVertex;
        currentVertexNeighbors = &VVSeam[nextVertex.v];

        // add current vertex to this seam
        thisSeam.push_back(currentVertex);

        // remove the previous vertex
        auto it = std::find(currentVertexNeighbors->begin(), currentVertexNeighbors->end(), prevVertex);
        assert(it != currentVertexNeighbors->end());
        currentVertexNeighbors->erase(it);

        if (currentVertexNeighbors->size() == 1 && !isStartVertex[currentVertex.v])
        {
          nextVertex = currentVertexNeighbors->front();
          currentVertexNeighbors->pop_front();
        }
        else
          break;
      }
      verticesPerSeam.push_back(thisSeam);
    }
  }

  return verticesPerSeam;
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::VertexIndexing<DerivedV, DerivedF>::InitSeamInfo()
{
  auto verticesPerSeam = GetVerticesPerSeam();
  Handle_SystemInfo.EdgeSeamInfo.clear();
  int integerVar = 0;
  // Loop over each seam
  for(auto seam : verticesPerSeam){
    //choose initial side of the seam such that the start vertex corresponds to Fcut(f, k) and the end vertex corresponds to Fcut(f, (k+1)%3) and not vice versa.
    int priorVertexIdx;
    if(seam.size() > 2){
      auto v1 = seam[1];
      auto v2 = seam[2];
      if(Fcut(v1.f0, (v1.k0+1) % 3) == Fcut(v2.f0, v2.k0) || Fcut(v1.f0, (v1.k0+1) % 3) == Fcut(v2.f1, v2.k1)){
        priorVertexIdx = Fcut(v1.f0, v1.k0);
      }
      else{
        priorVertexIdx = Fcut(v1.f1, v1.k1);
        assert(Fcut(v1.f1, (v1.k1+1) % 3) == Fcut(v2.f0, v2.k0) || Fcut(v1.f1, (v1.k1+1) % 3) == Fcut(v2.f1, v2.k1));
      }
    }
    else{
      auto v1 = seam[1];
      priorVertexIdx = Fcut(v1.f0, v1.k0);
    }

    // Loop over each vertex of the seam
    for(auto it=seam.begin()+1; it != seam.end(); ++it){
      auto vertex = *it;
      // choose the correct side of the seam
      int f,k,ff,kk;
      if(priorVertexIdx == Fcut(vertex.f0, vertex.k0)){
        f = vertex.f0; ff = vertex.f1;
        k = vertex.k0; kk = vertex.k1;
      }
      else{
        f = vertex.f1; ff = vertex.f0;
        k = vertex.k1; kk = vertex.k0;
        assert(priorVertexIdx == Fcut(vertex.f1, vertex.k1));
      }

      int vtx0,vtx0p,vtx1,vtx1p;
      unsigned char MM;
      GetSeamInfo(f,ff,k,vtx0,vtx1,vtx0p,vtx1p,MM);
      Handle_SystemInfo.EdgeSeamInfo.push_back(SeamInfo(vtx0,vtx0p,MM,integerVar));
      if(it == seam.end() -1){
        Handle_SystemInfo.EdgeSeamInfo.push_back(SeamInfo(vtx1,vtx1p,MM,integerVar));
      }
      priorVertexIdx = vtx1;
    }
    // use the same integer for each seam
    integerVar++;
  }
  Handle_SystemInfo.num_integer_cuts = integerVar;

#ifndef NDEBUG
  int totalNVerticesOnSeams = 0;
  for(auto seam : verticesPerSeam){
    totalNVerticesOnSeams += seam.size();
  }
  assert(Handle_SystemInfo.EdgeSeamInfo.size() == totalNVerticesOnSeams);
#endif
}



template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::SolvePoisson(Eigen::VectorXd Stiffness,
                                                                     double vector_field_scale,
                                                                     double grid_res,
                                                                     bool direct_round,
                                                                     int localIter,
                                                                     bool _integer_rounding,
                                                                     bool _singularity_rounding,
                                                                     std::vector<int> roundVertices,
                                                                     std::vector<std::vector<int> > hardFeatures)
{
  Handle_Stiffness = Stiffness;

  //initialization of flags and data structures
  integer_rounding=_integer_rounding;

  ids_to_round.clear();

  clearUserConstraint();
  // copy the user constraints number
  for (size_t i = 0; i < hardFeatures.size(); ++i)
  {
    addSharpEdgeConstraint(hardFeatures[i][0],hardFeatures[i][1]);
  }

  ///Initializing Matrix

  int t0=clock();

  ///initialize the matrix ALLOCATING SPACE
  InitMatrix();
  if (DEBUGPRINT)
    printf("\n ALLOCATED THE MATRIX \n");

  ///build the laplacian system
  BuildLaplacianMatrix(vector_field_scale);

  // add seam constraints
  BuildSeamConstraintsExplicitTranslation();

  // add user defined constraints
  BuildUserDefinedConstraints();

  ////add the lagrange multiplier
  FixBlockedVertex();

  if (DEBUGPRINT)
    printf("\n BUILT THE MATRIX \n");

  if (integer_rounding)
    AddToRoundVertices(roundVertices);

  if (_singularity_rounding)
    AddSingularityRound();

  int t1=clock();
  if (DEBUGPRINT) printf("\n time:%d \n",t1-t0);
  if (DEBUGPRINT) printf("\n SOLVING \n");

  MixedIntegerSolve(grid_res,direct_round,localIter);

  int t2=clock();
  if (DEBUGPRINT) printf("\n time:%d \n",t2-t1);
  if (DEBUGPRINT) printf("\n ASSIGNING COORDS \n");

  MapCoords();

  int t3=clock();
  if (DEBUGPRINT) printf("\n time:%d \n",t3-t2);
  if (DEBUGPRINT) printf("\n FINISHED \n");
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>
::PoissonSolver(const Eigen::PlainObjectBase<DerivedV> &_V,
                const Eigen::PlainObjectBase<DerivedF> &_F,
                const Eigen::PlainObjectBase<DerivedV> &_Vcut,
                const Eigen::PlainObjectBase<DerivedF> &_Fcut,
                const Eigen::PlainObjectBase<DerivedF> &_TT,
                const Eigen::PlainObjectBase<DerivedF> &_TTi,
                const Eigen::PlainObjectBase<DerivedV> &_PD1,
                const Eigen::PlainObjectBase<DerivedV> &_PD2,
                const Eigen::Matrix<int, Eigen::Dynamic, 1>&_Handle_Singular,
                const MeshSystemInfo &_Handle_SystemInfo
):
V(_V),
F(_F),
Vcut(_Vcut),
Fcut(_Fcut),
TT(_TT),
TTi(_TTi),
PD1(_PD1),
PD2(_PD2),
Handle_Singular(_Handle_Singular),
Handle_SystemInfo(_Handle_SystemInfo)
{
  UV        = Eigen::MatrixXd(V.rows(),2);
  WUV       = Eigen::MatrixXd(F.rows(),6);
  UV_out    = Eigen::MatrixXd(Vcut.rows(),2);
  igl::vertex_triangle_adjacency(V,F,VF,VFi);
}

///START COMMON MATH FUNCTIONS
///return the complex encoding the rotation
///for a given missmatch interval
template <typename DerivedV, typename DerivedF>
IGL_INLINE std::complex<double> igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::GetRotationComplex(int interval)
{
  assert((interval>=0)&&(interval<4));

  switch(interval)
  {
    case 0:return std::complex<double>(1,0);
    case 1:return std::complex<double>(0,1);
    case 2:return std::complex<double>(-1,0);
    default:return std::complex<double>(0,-1);
  }
}

///END COMMON MATH FUNCTIONS

///START FIXING VERTICES
///set a given vertex as fixed
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::AddFixedVertex(int v)
{
  n_fixed_vars++;
  Hard_constraints.push_back(v);
}

///find vertex to fix in case we're using
///a vector field NB: multiple components not handled
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::FindFixedVertField()
{
  Hard_constraints.clear();

  n_fixed_vars=0;
  //fix the first singularity
  for (unsigned int v=0;v<V.rows();v++)
  {
    if (Handle_Singular(v))
    {
      AddFixedVertex(v);
      UV.row(v) << 0,0;
      return;
    }
  }

  ///if anything fixed fix the first
  AddFixedVertex(0);
  UV.row(0) << 0,0;
  std::cerr << "No vertices to fix, I am fixing the first vertex to the origin!" << std::endl;
}

///find hard constraint depending if using or not
///a vector field
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::FindFixedVert()
{
  Hard_constraints.clear();
  FindFixedVertField();
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE int igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::GetFirstVertexIndex(int v)
{
  return Fcut(VF[v][0],VFi[v][0]);
}

///fix the vertices which are flagged as fixed
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::FixBlockedVertex()
{
  int offset_row = num_cut_constraint*2;

  unsigned int constr_num = 0;
  for (unsigned int i=0;i<Hard_constraints.size();i++)
  {
    int v = Hard_constraints[i];

    ///get first index of the vertex that must blocked
    //int index=v->vertex_index[0];
    int index = GetFirstVertexIndex(v);

    ///multiply times 2 because of uv
    int indexvert = index*2;

    ///find the first free row to add the constraint
    int indexRow = (offset_row+constr_num*2);
    int indexCol = indexRow;

    ///add fixing constraint LHS
    Constraints.coeffRef(indexRow,  indexvert)   += 1;
    Constraints.coeffRef(indexRow+1,indexvert+1) += 1;

    ///add fixing constraint RHS
    constraints_rhs[indexCol]   = UV(v,0);
    constraints_rhs[indexCol+1] = UV(v,1);

    constr_num++;
  }
  assert(constr_num==n_fixed_vars);
}
///END FIXING VERTICES

///HANDLING SINGULARITY
//set the singularity round to integer location
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::AddSingularityRound()
{
  for (unsigned int v=0;v<V.rows();v++)
  {
    if (Handle_Singular(v))
    {
      int index0=GetFirstVertexIndex(v);
      ids_to_round.push_back( index0*2   );
      ids_to_round.push_back((index0*2)+1);
    }
  }
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::AddToRoundVertices(std::vector<int> ids)
{
  for (size_t i = 0; i < ids.size(); ++i)
  {
    if (ids[i] < 0 || ids[i] >= V.rows())
      std::cerr << "WARNING: Ignored round vertex constraint, vertex " << ids[i] << " does not exist in the mesh." << std::endl;
    int index0 = GetFirstVertexIndex(ids[i]);
    ids_to_round.push_back( index0*2   );
    ids_to_round.push_back((index0*2)+1);
  }
}

///START GENERIC SYSTEM FUNCTIONS
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::BuildLaplacianMatrix(double vfscale)
{
  Eigen::VectorXi idx  = igl::LinSpaced<Eigen::VectorXi >(Vcut.rows(), 0, 2*Vcut.rows()-2);
  Eigen::VectorXi idx2 = igl::LinSpaced<Eigen::VectorXi >(Vcut.rows(), 1, 2*Vcut.rows()-1);

  // get gradient matrix
  Eigen::SparseMatrix<double> G(Fcut.rows() * 3, Vcut.rows());
  igl::grad(Vcut, Fcut, G);

  // get triangle weights
  Eigen::VectorXd dblA(Fcut.rows());
  igl::doublearea(Vcut, Fcut, dblA);

  // compute intermediate result
  Eigen::SparseMatrix<double> G2;
  G2 = G.transpose() * dblA.replicate<3,1>().asDiagonal() * Handle_Stiffness.replicate<3,1>().asDiagonal();

  ///  Compute LHS
  Eigen::SparseMatrix<double> Cotmatrix;
  Cotmatrix = 0.5 * G2 * G;
  igl::slice_into(Cotmatrix, idx,  idx,  Lhs);
  igl::slice_into(Cotmatrix, idx2, idx2, Lhs);

  /// Compute RHS
  // reshape nrosy vectors
  const Eigen::MatrixXd u = Eigen::Map<const Eigen::MatrixXd>(PD1.data(),Fcut.rows()*3,1); // this mimics a reshape at the cost of a copy.
  const Eigen::MatrixXd v = Eigen::Map<const Eigen::MatrixXd>(PD2.data(),Fcut.rows()*3,1); // this mimics a reshape at the cost of a copy.

  // multiply with weights
  Eigen::VectorXd rhs1 =  G2 * u * 0.5 * vfscale;
  Eigen::VectorXd rhs2 = -G2 * v * 0.5 * vfscale;
  igl::slice_into(rhs1, idx,  1, rhs);
  igl::slice_into(rhs2, idx2, 1, rhs);
}

///find different sized of the system
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::FindSizes()
{
  ///find the vertex that need to be fixed
  FindFixedVert();

  ///REAL PART
  n_vert_vars = Handle_SystemInfo.num_vert_variables;

  ///INTEGER PART
  ///the total number of integer variables
  n_integer_vars = Handle_SystemInfo.num_integer_cuts;

  ///CONSTRAINT PART
  num_cut_constraint = Handle_SystemInfo.EdgeSeamInfo.size();

  num_constraint_equations = num_cut_constraint * 2 + n_fixed_vars * 2 + num_userdefined_constraint;

  ///total variable of the system
  num_total_vars = (n_vert_vars+n_integer_vars) * 2;

  ///initialize matrix size

  if (DEBUGPRINT)     printf("\n*** SYSTEM VARIABLES *** \n");
  if (DEBUGPRINT)     printf("* NUM REAL VERTEX VARIABLES %d \n",n_vert_vars);

  if (DEBUGPRINT)     printf("\n*** INTEGER VARIABLES *** \n");
  if (DEBUGPRINT)     printf("* NUM INTEGER VARIABLES %d \n",(int)n_integer_vars);

  if (DEBUGPRINT)     printf("\n*** CONSTRAINTS *** \n ");
  if (DEBUGPRINT)     printf("* NUM FIXED CONSTRAINTS %d\n",n_fixed_vars);
  if (DEBUGPRINT)     printf("* NUM CUTS CONSTRAINTS %d\n",num_cut_constraint);
  if (DEBUGPRINT)     printf("* NUM USER DEFINED CONSTRAINTS %d\n",num_userdefined_constraint);

  if (DEBUGPRINT)     printf("\n*** TOTAL SIZE *** \n");
  if (DEBUGPRINT)     printf("* TOTAL VARIABLE SIZE (WITH INTEGER TRASL) %d \n",num_total_vars);
  if (DEBUGPRINT)     printf("* TOTAL CONSTRAINTS %d \n",num_constraint_equations);
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::AllocateSystem()
{
  Lhs.resize(n_vert_vars * 2, n_vert_vars * 2);
  Constraints.resize(num_constraint_equations, num_total_vars);
  rhs.resize(n_vert_vars * 2);
  constraints_rhs.resize(num_constraint_equations);

  printf("\n INITIALIZED SPARSE MATRIX OF %d x %d \n",n_vert_vars*2, n_vert_vars*2);
  printf("\n INITIALIZED SPARSE MATRIX OF %d x %d \n",num_constraint_equations, num_total_vars);
  printf("\n INITIALIZED VECTOR OF %d x 1 \n",n_vert_vars*2);
  printf("\n INITIALIZED VECTOR OF %d x 1 \n",num_constraint_equations);
}

///intitialize the whole matrix
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::InitMatrix()
{
  FindSizes();
  AllocateSystem();
}

///map back coordinates after that
///the system has been solved
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::MapCoords()
{
  ///map coords to faces
  for (unsigned int f=0;f<Fcut.rows();f++)
  {

    for (int k=0;k<3;k++)
    {
      //get the index of the variable in the system
      int indexUV = Fcut(f,k);
      ///then get U and V coords
      double U=X[indexUV*2];
      double V=X[indexUV*2+1];

      WUV(f,k*2 + 0) = U;
      WUV(f,k*2 + 1) = V;
    }

  }

  for(int i = 0; i < Vcut.rows(); i++){
    UV_out(i,0) = X[i*2];
    UV_out(i,1) = X[i*2+1];
  }
}

///END GENERIC SYSTEM FUNCTIONS

///set the constraints for the inter-range cuts
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::BuildSeamConstraintsExplicitTranslation()
{
  ///current constraint row
  int constr_row = 0;

  for (unsigned int i=0; i<num_cut_constraint; i++)
  {
    unsigned char interval = Handle_SystemInfo.EdgeSeamInfo[i].MMatch;
    if (interval==1)
      interval=3;
    else
      if(interval==3)
        interval=1;

    int p0  = Handle_SystemInfo.EdgeSeamInfo[i].v0;
    int p0p = Handle_SystemInfo.EdgeSeamInfo[i].v0p;

    std::complex<double> rot = GetRotationComplex(interval);

    ///get the integer variable
    int integerVar = n_vert_vars + Handle_SystemInfo.EdgeSeamInfo[i].integerVar;

    if (integer_rounding)
    {
      ids_to_round.push_back(integerVar*2);
      ids_to_round.push_back(integerVar*2+1);
    }

    // cross boundary compatibility conditions
    Constraints.coeffRef(constr_row,   2*p0)   +=  rot.real();
    Constraints.coeffRef(constr_row,   2*p0+1) += -rot.imag();
    Constraints.coeffRef(constr_row+1, 2*p0)   +=  rot.imag();
    Constraints.coeffRef(constr_row+1, 2*p0+1) +=  rot.real();

    Constraints.coeffRef(constr_row,   2*p0p)   += -1;
    Constraints.coeffRef(constr_row+1, 2*p0p+1) += -1;

    Constraints.coeffRef(constr_row,   2*integerVar)   += 1;
    Constraints.coeffRef(constr_row+1, 2*integerVar+1) += 1;

    constraints_rhs[constr_row]   = 0;
    constraints_rhs[constr_row+1] = 0;

    constr_row += 2;
  }

}

///set the constraints for the inter-range cuts
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::BuildUserDefinedConstraints()
{
  /// the user defined constraints are at the end
  int offset_row = num_cut_constraint*2 + n_fixed_vars*2;

  ///current constraint row
  int constr_row = offset_row;

  assert(num_userdefined_constraint == userdefined_constraints.size());

  for (unsigned int i=0; i<num_userdefined_constraint; i++)
  {
    for (unsigned int j=0; j<userdefined_constraints[i].size()-1; ++j)
    {
      Constraints.coeffRef(constr_row, j) = userdefined_constraints[i][j];
    }

    constraints_rhs[constr_row] = userdefined_constraints[i][userdefined_constraints[i].size()-1];
    constr_row +=1;
  }
}

///call of the mixed integer solver
template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::MixedIntegerSolve(double cone_grid_res,
                                                                          bool direct_round,
                                                                          int localIter)
{
  X = std::vector<double>((n_vert_vars+n_integer_vars)*2);
  if (DEBUGPRINT)
    printf("\n ALLOCATED X \n");

  ///variables part
  int ScalarSize = n_vert_vars*2;
  int SizeMatrix = (n_vert_vars+n_integer_vars)*2;

  ///matrix A
  gmm::col_matrix< gmm::wsvector< double > > A(SizeMatrix,SizeMatrix); // lhs matrix variables

  ///constraints part
  int CsizeX = num_constraint_equations;
  int CsizeY = SizeMatrix+1;
  gmm::row_matrix< gmm::wsvector< double > > C(CsizeX,CsizeY); // constraints

  if (DEBUGPRINT)
    printf("\n ALLOCATED QMM STRUCTURES \n");

  std::vector<double> B(SizeMatrix,0);  // rhs

  if (DEBUGPRINT)
    printf("\n ALLOCATED RHS STRUCTURES \n");

  //// copy LHS
  for (int k=0; k < Lhs.outerSize(); ++k){
    for (Eigen::SparseMatrix<double>::InnerIterator it(Lhs,k); it; ++it){
      int row = it.row();
      int col = it.col();
      A(row, col) += it.value();
    }
  }
  //// copy Constraints
  for (int k=0; k < Constraints.outerSize(); ++k){
    for (Eigen::SparseMatrix<double>::InnerIterator it(Constraints,k); it; ++it){
      int row = it.row();
      int col = it.col();
      C(row, col) += it.value();
    }
  }

  if (DEBUGPRINT)
    printf("\n SET %d INTEGER VALUES \n",n_integer_vars);

  ///add penalization term for integer variables
  double penalization = 0.000001;
  int offline_index   = ScalarSize;
  for(unsigned int i = 0; i < (n_integer_vars)*2; ++i)
  {
    int index=offline_index+i;
    A(index,index)=penalization;
  }

  if (DEBUGPRINT)
    printf("\n SET RHS \n");

  // copy RHS
  for(int i = 0; i < (int)ScalarSize; ++i)
  {
    B[i] = rhs[i] * cone_grid_res;
  }

  // copy constraint RHS
  if (DEBUGPRINT)
    printf("\n SET %d CONSTRAINTS \n",num_constraint_equations);

  for(unsigned int i = 0; i < num_constraint_equations; ++i)
  {
    C(i, SizeMatrix) = -constraints_rhs[i] * cone_grid_res;
  }

  COMISO::ConstrainedSolver solver;

  solver.misolver().set_local_iters(localIter);

  solver.misolver().set_direct_rounding(direct_round);

  std::sort(ids_to_round.begin(),ids_to_round.end());
  std::vector<int>::iterator new_end=std::unique(ids_to_round.begin(),ids_to_round.end());
  int dist=distance(ids_to_round.begin(),new_end);
  ids_to_round.resize(dist);

  solver.solve( C, A, X, B, ids_to_round, 0.0, false, false);
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::clearUserConstraint()
{
  num_userdefined_constraint = 0;
  userdefined_constraints.clear();
}

template <typename DerivedV, typename DerivedF>
IGL_INLINE void igl::copyleft::comiso::PoissonSolver<DerivedV, DerivedF>::addSharpEdgeConstraint(int fid, int vid)
{
  // prepare constraint
  std::vector<int> c(Handle_SystemInfo.num_vert_variables*2 + 1);

  for (size_t i = 0; i < c.size(); ++i)
  {
    c[i] = 0;
  }

  int v1 = Fcut(fid,vid);
  int v2 = Fcut(fid,(vid+1)%3);

  Eigen::Matrix<typename DerivedV::Scalar, 3, 1> e = Vcut.row(v2) - Vcut.row(v1);
  e = e.normalized();

  double d1 = fabs(e.dot(PD1.row(fid).normalized()));
  double d2 = fabs(e.dot(PD2.row(fid).normalized()));

  int offset = 0;

  if (d1>d2)
    offset = 1;

  ids_to_round.push_back((v1 * 2) + offset);
  ids_to_round.push_back((v2 * 2) + offset);

  // add constraint
  c[(v1 * 2) + offset] =  1;
  c[(v2 * 2) + offset] = -1;

  // add to the user-defined constraints
  num_userdefined_constraint++;
  userdefined_constraints.push_back(c);

}



template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE igl::copyleft::comiso::MIQ_class<DerivedV, DerivedF, DerivedU>::MIQ_class(const Eigen::PlainObjectBase<DerivedV> &V_,
                                                                   const Eigen::PlainObjectBase<DerivedF> &F_,
                                                                   const Eigen::PlainObjectBase<DerivedV> &PD1_combed,
                                                                   const Eigen::PlainObjectBase<DerivedV> &PD2_combed,
                                                                   const Eigen::Matrix<int, Eigen::Dynamic, 3> &Handle_MMatch,
                                                                   const Eigen::Matrix<int, Eigen::Dynamic, 1> &Handle_Singular,
                                                                   const Eigen::Matrix<int, Eigen::Dynamic, 3> &Handle_Seams,
                                                                   Eigen::PlainObjectBase<DerivedU> &UV,
                                                                   Eigen::PlainObjectBase<DerivedF> &FUV,
                                                                   double GradientSize,
                                                                   double Stiffness,
                                                                   bool DirectRound,
                                                                   int iter,
                                                                   int localIter,
                                                                   bool DoRound,
                                                                   bool SingularityRound,
                                                                   std::vector<int> roundVertices,
                                                                   std::vector<std::vector<int> > hardFeatures):
V(V_),
F(F_)
{
  igl::cut_mesh(V, F, Handle_Seams, Vcut, Fcut);

  igl::local_basis(V,F,B1,B2,B3);
  igl::triangle_triangle_adjacency(F,TT,TTi);

  // Prepare indexing for the linear system
  VertexIndexing<DerivedV, DerivedF> VInd(V, F, Vcut, Fcut, TT, TTi, Handle_MMatch, Handle_Singular, Handle_Seams);

  VInd.InitSeamInfo();

  // Assemble the system and solve
  PoissonSolver<DerivedV, DerivedF> PSolver(V,
                                            F,
                                            Vcut,
                                            Fcut,
                                            TT,
                                            TTi,
                                            PD1_combed,
                                            PD2_combed,
                                            Handle_Singular,
                                            VInd.Handle_SystemInfo);
  Handle_Stiffness = Eigen::VectorXd::Constant(F.rows(),1);


  if (iter > 0) // do stiffening
  {
    for (int i=0;i<iter;i++)
    {
      PSolver.SolvePoisson(Handle_Stiffness, GradientSize,1.f,DirectRound,localIter,DoRound,SingularityRound,roundVertices,hardFeatures);
      int nflips=NumFlips(PSolver.WUV);
      bool folded = updateStiffeningJacobianDistorsion(GradientSize,PSolver.WUV);
      printf("ITERATION %d FLIPS %d \n",i,nflips);
      if (!folded)break;
    }
  }
  else
  {
    PSolver.SolvePoisson(Handle_Stiffness,GradientSize,1.f,DirectRound,localIter,DoRound,SingularityRound,roundVertices,hardFeatures);
  }

  int nflips=NumFlips(PSolver.WUV);
  printf("**** END OPTIMIZING #FLIPS %d  ****\n",nflips);

  UV_out = PSolver.UV_out;
  FUV_out = PSolver.Fcut;
  fflush(stdout);
}

template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE void igl::copyleft::comiso::MIQ_class<DerivedV, DerivedF, DerivedU>::extractUV(Eigen::PlainObjectBase<DerivedU> &UV_out,
                                                                        Eigen::PlainObjectBase<DerivedF> &FUV_out)
{
  UV_out = this->UV_out;
  FUV_out = this->FUV_out;
}

template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE int igl::copyleft::comiso::MIQ_class<DerivedV, DerivedF, DerivedU>::NumFlips(const Eigen::MatrixXd& WUV)
{
  int numFl=0;
  for (unsigned int i=0;i<F.rows();i++)
  {
    if (IsFlipped(i, WUV))
      numFl++;
  }
  return numFl;
}

template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE double igl::copyleft::comiso::MIQ_class<DerivedV, DerivedF, DerivedU>::Distortion(int f, double h, const Eigen::MatrixXd& WUV)
{
  assert(h > 0);

  Eigen::Vector2d uv0,uv1,uv2;

  uv0 << WUV(f,0), WUV(f,1);
  uv1 << WUV(f,2), WUV(f,3);
  uv2 << WUV(f,4), WUV(f,5);

  Eigen::Matrix<typename DerivedV::Scalar, 3, 1> p0 = Vcut.row(Fcut(f,0));
  Eigen::Matrix<typename DerivedV::Scalar, 3, 1> p1 = Vcut.row(Fcut(f,1));
  Eigen::Matrix<typename DerivedV::Scalar, 3, 1> p2 = Vcut.row(Fcut(f,2));

  Eigen::Matrix<typename DerivedV::Scalar, 3, 1> norm = (p1 - p0).cross(p2 - p0);
  double area2 = norm.norm();
  double area2_inv  = 1.0 / area2;
  norm *= area2_inv;

  if (area2 > 0)
  {
    // Singular values of the Jacobian
    Eigen::Matrix<typename DerivedV::Scalar, 3, 1> neg_t0 = norm.cross(p2 - p1);
    Eigen::Matrix<typename DerivedV::Scalar, 3, 1> neg_t1 = norm.cross(p0 - p2);
    Eigen::Matrix<typename DerivedV::Scalar, 3, 1> neg_t2 = norm.cross(p1 - p0);

    Eigen::Matrix<typename DerivedV::Scalar, 3, 1> diffu =  (neg_t0 * uv0(0) +neg_t1 *uv1(0) +  neg_t2 * uv2(0) )*area2_inv;
    Eigen::Matrix<typename DerivedV::Scalar, 3, 1> diffv = (neg_t0 * uv0(1) + neg_t1*uv1(1) +  neg_t2*uv2(1) )*area2_inv;

    // first fundamental form
    double I00 = diffu.dot(diffu);  // guaranteed non-neg
    double I01 = diffu.dot(diffv);  // I01 = I10
    double I11 = diffv.dot(diffv);  // guaranteed non-neg

    // eigenvalues of a 2x2 matrix
    // [a00 a01]
    // [a10 a11]
    // 1/2 * [ (a00 + a11) +/- sqrt((a00 - a11)^2 + 4 a01 a10) ]
    double trI = I00 + I11;                     // guaranteed non-neg
    double diffDiag = I00 - I11;                // guaranteed non-neg
    double sqrtDet = sqrt(std::max(0.0, diffDiag*diffDiag +
                                   4 * I01 * I01)); // guaranteed non-neg
    double sig1 = 0.5 * (trI + sqrtDet); // higher singular value
    double sig2 = 0.5 * (trI - sqrtDet); // lower singular value

    // Avoid sig2 < 0 due to numerical error
    if (fabs(sig2) < 1.0e-8)
      sig2 = 0;

    assert(sig1 >= 0);
    assert(sig2 >= 0);

    if (sig2 < 0) {
      printf("Distortion will be NaN! sig1^2 is negative (%lg)\n",
             sig2);
    }

    // The singular values of the Jacobian are the sqrts of the
    // eigenvalues of the first fundamental form.
    sig1 = sqrt(sig1);
    sig2 = sqrt(sig2);

    // distortion
    double tao = IsFlipped(f,WUV) ? -1 : 1;
    double factor = tao / h;
    double lam = fabs(factor * sig1 - 1) + fabs(factor * sig2 - 1);
    return lam;
  }
  else {
    return 10; // something "large"
  }
}

////////////////////////////////////////////////////////////////////////////
// Approximate the distortion laplacian using a uniform laplacian on
//  the dual mesh:
//      ___________
//      \-1 / \-1 /
//       \ / 3 \ /
//        \-----/
//         \-1 /
//          \ /
//
//  @param[in]  f   facet on which to compute distortion laplacian
//  @param[in]  h   scaling factor applied to cross field
//  @return     distortion laplacian for f
///////////////////////////////////////////////////////////////////////////
template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE double igl::copyleft::comiso::MIQ_class<DerivedV, DerivedF, DerivedU>::LaplaceDistortion(const int f, double h, const Eigen::MatrixXd& WUV)
{
  double mydist = Distortion(f, h, WUV);
  double lapl=0;
  for (int i=0;i<3;i++)
  {
    if (TT(f,i) != -1)
      lapl += (mydist - Distortion(TT(f,i), h, WUV));
  }
  return lapl;
}

template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE bool igl::copyleft::comiso::MIQ_class<DerivedV, DerivedF, DerivedU>::updateStiffeningJacobianDistorsion(double grad_size, const Eigen::MatrixXd& WUV)
{
  bool flipped = NumFlips(WUV)>0;

  if (!flipped)
    return false;

  double maxL=0;
  double maxD=0;

  if (flipped)
  {
    const double c = 1.0;
    const double d = 5.0;

    for (unsigned int i = 0; i < Fcut.rows(); ++i)
    {
      double dist=Distortion(i,grad_size,WUV);
      if (dist > maxD)
        maxD=dist;

      double absLap=fabs(LaplaceDistortion(i, grad_size,WUV));
      if (absLap > maxL)
        maxL = absLap;

      double stiffDelta = std::min(c * absLap, d);

      Handle_Stiffness[i]+=stiffDelta;
    }
  }
  printf("Maximum Distorsion %4.4f \n",maxD);
  printf("Maximum Laplacian %4.4f \n",maxL);
  return flipped;
}

template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE bool igl::copyleft::comiso::MIQ_class<DerivedV, DerivedF, DerivedU>::IsFlipped(const Eigen::Vector2d &uv0,
                                                                        const Eigen::Vector2d &uv1,
                                                                        const Eigen::Vector2d &uv2)
{
  Eigen::Vector2d e0 = (uv1-uv0);
  Eigen::Vector2d e1 = (uv2-uv0);

  double Area = e0(0)*e1(1) - e0(1)*e1(0);
  return (Area<=0);
}

template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE bool igl::copyleft::comiso::MIQ_class<DerivedV, DerivedF, DerivedU>::IsFlipped(
  const int i, const Eigen::MatrixXd& WUV)
{
  Eigen::Vector2d uv0,uv1,uv2;
  uv0 << WUV(i,0), WUV(i,1);
  uv1 << WUV(i,2), WUV(i,3);
  uv2 << WUV(i,4), WUV(i,5);

  return (IsFlipped(uv0,uv1,uv2));
}




template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE void igl::copyleft::comiso::miq(
  const Eigen::PlainObjectBase<DerivedV> &V,
  const Eigen::PlainObjectBase<DerivedF> &F,
  const Eigen::PlainObjectBase<DerivedV> &PD1_combed,
  const Eigen::PlainObjectBase<DerivedV> &PD2_combed,
  const Eigen::Matrix<int, Eigen::Dynamic, 3> &Handle_MMatch,
  const Eigen::Matrix<int, Eigen::Dynamic, 1> &Handle_Singular,
  const Eigen::Matrix<int, Eigen::Dynamic, 3> &Handle_Seams,
  Eigen::PlainObjectBase<DerivedU> &UV,
  Eigen::PlainObjectBase<DerivedF> &FUV,
  double GradientSize,
  double Stiffness,
  bool DirectRound,
  int iter,
  int localIter,
  bool DoRound,
  bool SingularityRound,
  std::vector<int> roundVertices,
  std::vector<std::vector<int> > hardFeatures)
{
  GradientSize = GradientSize/(V.colwise().maxCoeff()-V.colwise().minCoeff()).norm();

  igl::copyleft::comiso::MIQ_class<DerivedV, DerivedF, DerivedU> miq(V,
    F,
    PD1_combed,
    PD2_combed,
    Handle_MMatch,
    Handle_Singular,
    Handle_Seams,
    UV,
    FUV,
    GradientSize,
    Stiffness,
    DirectRound,
    iter,
    localIter,
    DoRound,
    SingularityRound,
    roundVertices,
    hardFeatures);

  miq.extractUV(UV,FUV);
}

template <typename DerivedV, typename DerivedF, typename DerivedU>
IGL_INLINE void igl::copyleft::comiso::miq(
    const Eigen::PlainObjectBase<DerivedV> &V,
    const Eigen::PlainObjectBase<DerivedF> &F,
    const Eigen::PlainObjectBase<DerivedV> &PD1,
    const Eigen::PlainObjectBase<DerivedV> &PD2,
    Eigen::PlainObjectBase<DerivedU> &UV,
    Eigen::PlainObjectBase<DerivedF> &FUV,
    double GradientSize,
    double Stiffness,
    bool DirectRound,
    int iter,
    int localIter,
    bool DoRound,
    bool SingularityRound,
    std::vector<int> roundVertices,
    std::vector<std::vector<int> > hardFeatures)
{

  DerivedV BIS1, BIS2;
  igl::compute_frame_field_bisectors(V, F, PD1, PD2, BIS1, BIS2);

  DerivedV BIS1_combed, BIS2_combed;
  igl::comb_cross_field(V, F, BIS1, BIS2, BIS1_combed, BIS2_combed);

  DerivedF Handle_MMatch;
  igl::cross_field_missmatch(V, F, BIS1_combed, BIS2_combed, true, Handle_MMatch);

  Eigen::Matrix<int, Eigen::Dynamic, 1> isSingularity, singularityIndex;
  igl::find_cross_field_singularities(V, F, Handle_MMatch, isSingularity, singularityIndex);

  Eigen::Matrix<int, Eigen::Dynamic, 3> Handle_Seams;
  igl::cut_mesh_from_singularities(V, F, Handle_MMatch, Handle_Seams);

  DerivedV PD1_combed, PD2_combed;
  igl::comb_frame_field(V, F, PD1, PD2, BIS1_combed, BIS2_combed, PD1_combed, PD2_combed);

  igl::copyleft::comiso::miq(V,
           F,
           PD1_combed,
           PD2_combed,
           Handle_MMatch,
           isSingularity,
           Handle_Seams,
           UV,
           FUV,
           GradientSize,
           Stiffness,
           DirectRound,
           iter,
           localIter,
           DoRound,
           SingularityRound,
           roundVertices,
           hardFeatures);

}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::copyleft::comiso::miq<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&, double, double, bool, int, int, bool, bool, std::vector<int, std::allocator<int> >, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >);
template void igl::copyleft::comiso::miq<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<int, -1, 3, 0, -1, 3> const&, Eigen::Matrix<int, -1, 1, 0, -1, 1> const&, Eigen::Matrix<int, -1, 3, 0, -1, 3> const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, double, double, bool, int, int, bool, bool, std::vector<int, std::allocator<int> >, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >);
template void igl::copyleft::comiso::miq<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, double, double, bool, int, int, bool, bool, std::vector<int, std::allocator<int> >, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >);
#endif
