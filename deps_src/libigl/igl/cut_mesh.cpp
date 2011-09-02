// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2016 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include <igl/cut_mesh.h>
#include <igl/vertex_triangle_adjacency.h>
#include <igl/triangle_triangle_adjacency.h>
#include <igl/is_border_vertex.h>
#include <igl/HalfEdgeIterator.h>
#include <set>

// This file violates many of the libigl style guidelines.

namespace igl {


  template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
  class MeshCutterMini
  {
  public:
    // Input
    //mesh
    const Eigen::PlainObjectBase<DerivedV> &V;
    const Eigen::PlainObjectBase<DerivedF> &F;
    // TT is the same type as TTi? This is likely to break at some point
    const Eigen::PlainObjectBase<DerivedTT> &TT;
    const Eigen::PlainObjectBase<DerivedTT> &TTi;
    const std::vector<std::vector<VFType> >& VF;
    const std::vector<std::vector<VFType> >& VFi;
    const std::vector<bool> &V_border; // bool
    //edges to cut
    const Eigen::PlainObjectBase<DerivedC> &Handle_Seams; // 3 bool

    // total number of scalar variables
    int num_scalar_variables;

    // per face indexes of vertex in the solver
    DerivedF HandleS_Index;

    // per vertex variable indexes
    std::vector<std::vector<int> > HandleV_Integer;

    IGL_INLINE MeshCutterMini(
      const Eigen::PlainObjectBase<DerivedV> &_V,
      const Eigen::PlainObjectBase<DerivedF> &_F,
      const Eigen::PlainObjectBase<DerivedTT> &_TT,
      const Eigen::PlainObjectBase<DerivedTT> &_TTi,
      const std::vector<std::vector<VFType> > &_VF,
      const std::vector<std::vector<VFType> > &_VFi,
      const std::vector<bool> &_V_border,
      const Eigen::PlainObjectBase<DerivedC> &_Handle_Seams);

    // vertex to variable mapping
    // initialize the mapping for a given sampled mesh
    IGL_INLINE void InitMappingSeam();

  private:

    IGL_INLINE void FirstPos(const int v, int &f, int &edge);

    IGL_INLINE int AddNewIndex(const int v0);

    IGL_INLINE bool IsSeam(const int f0, const int f1);

    // find initial position of the pos to
    // assing face to vert inxex correctly
    IGL_INLINE void FindInitialPos(const int vert, int &edge, int &face);


    // initialize the mapping given an initial pos
    // whih must be initialized with FindInitialPos
    IGL_INLINE void MapIndexes(const int  vert, const int edge_init, const int f_init);

    // initialize the mapping for a given vertex
    IGL_INLINE void InitMappingSeam(const int vert);

  };
}


template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
IGL_INLINE igl::MeshCutterMini<DerivedV, DerivedF, VFType, DerivedTT, DerivedC>::
MeshCutterMini(
  const Eigen::PlainObjectBase<DerivedV> &_V,
  const Eigen::PlainObjectBase<DerivedF> &_F,
  const Eigen::PlainObjectBase<DerivedTT> &_TT,
  const Eigen::PlainObjectBase<DerivedTT> &_TTi,
  const std::vector<std::vector<VFType> > &_VF,
  const std::vector<std::vector<VFType> > &_VFi,
  const std::vector<bool> &_V_border,
  const Eigen::PlainObjectBase<DerivedC> &_Handle_Seams):
  V(_V),
  F(_F),
  TT(_TT),
  TTi(_TTi),
  VF(_VF),
  VFi(_VFi),
  V_border(_V_border),
  Handle_Seams(_Handle_Seams)
{
  num_scalar_variables=0;
  HandleS_Index.setConstant(F.rows(),3,-1);
  HandleV_Integer.resize(V.rows());
}


template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
IGL_INLINE void igl::MeshCutterMini<DerivedV, DerivedF, VFType, DerivedTT, DerivedC>::
FirstPos(const int v, int &f, int &edge)
{
  f    = VF[v][0];  // f=v->cVFp();
  edge = VFi[v][0]; // edge=v->cVFi();
}

template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
IGL_INLINE int igl::MeshCutterMini<DerivedV, DerivedF, VFType, DerivedTT, DerivedC>::
AddNewIndex(const int v0)
{
  num_scalar_variables++;
  HandleV_Integer[v0].push_back(num_scalar_variables);
  return num_scalar_variables;
}

template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
IGL_INLINE bool igl::MeshCutterMini<DerivedV, DerivedF, VFType, DerivedTT, DerivedC>::
IsSeam(const int f0, const int f1)
{
  for (int i=0;i<3;i++)
  {
    int f_clos = TT(f0,i);

    if (f_clos == -1)
      continue; ///border

    if (f_clos == f1)
      return(Handle_Seams(f0,i));
  }
  assert(0);
  return false;
}

///find initial position of the pos to
// assing face to vert inxex correctly
template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
IGL_INLINE void igl::MeshCutterMini<DerivedV, DerivedF, VFType, DerivedTT, DerivedC>::
FindInitialPos(const int vert,
               int &edge,
               int &face)
{
  int f_init;
  int edge_init;
  FirstPos(vert,f_init,edge_init); // todo manually the function
  igl::HalfEdgeIterator<DerivedF,DerivedTT,DerivedTT> VFI(F,TT,TTi,f_init,edge_init);

  bool vertexB = V_border[vert];
  bool possible_split=false;
  bool complete_turn=false;
  do
  {
    int curr_f = VFI.Fi();
    int curr_edge=VFI.Ei();
    VFI.NextFE();
    int next_f=VFI.Fi();
    ///test if I've just crossed a border
    bool on_border=(TT(curr_f,curr_edge)==-1);
    //bool mismatch=false;
    bool seam=false;

    ///or if I've just crossed a seam
    ///if I'm on a border I MUST start from the one next t othe border
    if (!vertexB)
      //seam=curr_f->IsSeam(next_f);
      seam=IsSeam(curr_f,next_f);
    //    if (vertexB)
    //      assert(!Handle_Singular(vert));
    //    ;
    //assert(!vert->IsSingular());
    possible_split=((on_border)||(seam));
    complete_turn = next_f == f_init;
  } while ((!possible_split)&&(!complete_turn));
  face=VFI.Fi();
  edge=VFI.Ei();
}



///initialize the mapping given an initial pos
///whih must be initialized with FindInitialPos
template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
IGL_INLINE void igl::MeshCutterMini<DerivedV, DerivedF, VFType, DerivedTT, DerivedC>::
MapIndexes(const int  vert,
           const int edge_init,
           const int f_init)
{
  ///check that is not on border..
  ///in such case maybe it's non manyfold
  ///insert an initial index
  int curr_index=AddNewIndex(vert);
  ///and initialize the jumping pos
  igl::HalfEdgeIterator<DerivedF,DerivedTT,DerivedTT> VFI(F,TT,TTi,f_init,edge_init);
  bool complete_turn=false;
  do
  {
    int curr_f = VFI.Fi();
    int curr_edge = VFI.Ei();
    ///assing the current index
    HandleS_Index(curr_f,curr_edge) = curr_index;
    VFI.NextFE();
    int next_f = VFI.Fi();
    ///test if I've finiseh with the face exploration
    complete_turn = (next_f==f_init);
    ///or if I've just crossed a mismatch
    if (!complete_turn)
    {
      bool seam=false;
      //seam=curr_f->IsSeam(next_f);
      seam=IsSeam(curr_f,next_f);
      if (seam)
      {
        ///then add a new index
        curr_index=AddNewIndex(vert);
      }
    }
  } while (!complete_turn);
}

///initialize the mapping for a given vertex
template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
IGL_INLINE void igl::MeshCutterMini<DerivedV, DerivedF, VFType, DerivedTT, DerivedC>::
InitMappingSeam(const int vert)
{
  ///first rotate until find the first pos after a mismatch
  ///or a border or return to the first position...
  int f_init = VF[vert][0];
  int indexE = VFi[vert][0];

  igl::HalfEdgeIterator<DerivedF,DerivedTT,DerivedTT> VFI(F,TT,TTi,f_init,indexE);

  int edge_init;
  int face_init;
  FindInitialPos(vert,edge_init,face_init);
  MapIndexes(vert,edge_init,face_init);
}

///vertex to variable mapping
///initialize the mapping for a given sampled mesh
template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
IGL_INLINE void igl::MeshCutterMini<DerivedV, DerivedF, VFType, DerivedTT, DerivedC>::
InitMappingSeam()
{
  num_scalar_variables=-1;
  for (unsigned int i=0;i<V.rows();i++)
    InitMappingSeam(i);

  for (unsigned int j=0;j<V.rows();j++)
    assert(HandleV_Integer[j].size()>0);
}


template <typename DerivedV, typename DerivedF, typename VFType, typename DerivedTT, typename DerivedC>
IGL_INLINE void igl::cut_mesh(
  const Eigen::PlainObjectBase<DerivedV> &V,
  const Eigen::PlainObjectBase<DerivedF> &F,
  const std::vector<std::vector<VFType> >& VF,
  const std::vector<std::vector<VFType> >& VFi,
  const Eigen::PlainObjectBase<DerivedTT>& TT,
  const Eigen::PlainObjectBase<DerivedTT>& TTi,
  const std::vector<bool> &V_border,
  const Eigen::PlainObjectBase<DerivedC> &cuts,
  Eigen::PlainObjectBase<DerivedV> &Vcut,
  Eigen::PlainObjectBase<DerivedF> &Fcut)
{
  //finding the cuts is done, now we need to actually generate a cut mesh
  igl::MeshCutterMini<DerivedV, DerivedF, VFType, DerivedTT, DerivedC> mc(V, F, TT, TTi, VF, VFi, V_border, cuts);
  mc.InitMappingSeam();

  Fcut = mc.HandleS_Index;
  //we have the faces, we need the vertices;
  int newNumV = Fcut.maxCoeff()+1;
  Vcut.setZero(newNumV,3);
  for (int vi=0; vi<V.rows(); ++vi)
    for (int i=0; i<mc.HandleV_Integer[vi].size();++i)
      Vcut.row(mc.HandleV_Integer[vi][i]) = V.row(vi);

  //ugly hack to fix some problematic cases (border vertex that is also on the boundary of the hole
  for (int fi =0; fi<Fcut.rows(); ++fi)
    for (int k=0; k<3; ++k)
      if (Fcut(fi,k)==-1)
      {
        //we need to add a vertex
        Fcut(fi,k) = newNumV;
        newNumV ++;
        Vcut.conservativeResize(newNumV, Eigen::NoChange);
        Vcut.row(newNumV-1) = V.row(F(fi,k));
      }


}


//Wrapper of the above with only vertices and faces as mesh input
template <typename DerivedV, typename DerivedF, typename DerivedC>
IGL_INLINE void igl::cut_mesh(
  const Eigen::PlainObjectBase<DerivedV> &V,
  const Eigen::PlainObjectBase<DerivedF> &F,
  const Eigen::PlainObjectBase<DerivedC> &cuts,
  Eigen::PlainObjectBase<DerivedV> &Vcut,
  Eigen::PlainObjectBase<DerivedF> &Fcut)
{
  std::vector<std::vector<int> > VF, VFi;
  igl::vertex_triangle_adjacency(V,F,VF,VFi);
  // Alec: Cast? Why? This is likely to break.
  Eigen::MatrixXd Vt = V;
  Eigen::MatrixXi Ft = F;
  Eigen::MatrixXi TT, TTi;
  igl::triangle_triangle_adjacency(Ft,TT,TTi);
  std::vector<bool> V_border = igl::is_border_vertex(V,F);
  igl::cut_mesh(V, F, VF, VFi, TT, TTi, V_border, cuts, Vcut, Fcut);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::cut_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, int, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, std::vector<bool, std::allocator<bool> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cut_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::cut_mesh<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> >&);
template void igl::cut_mesh<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 3, 0, -1, 3> >(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#endif
