// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Alec Jacobson <alecjacobson@gmail.com>
//                    Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
//
#include "mesh_boolean.h"
#include "assign_scalar.h"
#include "extract_cells.h"
#include "mesh_boolean_type_to_funcs.h"
#include "propagate_winding_numbers.h"
#include "relabel_small_immersed_cells.h"
#include "remesh_self_intersections.h"
#include "string_to_mesh_boolean_type.h"
#include "../../combine.h"
#include "../../PlainVector.h"
#include "../../PlainMatrix.h"
#include "../../cumsum.h"
#include "../../extract_manifold_patches.h"
#include "../../get_seconds.h"
#include "../../parallel_for.h"
#include "../../remove_unreferenced.h"
#include "../../resolve_duplicated_faces.h"
#include "../../unique_edge_map.h"
#include "../../unique_simplices.h"
#include "../../C_STR.h"

#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <algorithm>

//#define MESH_BOOLEAN_TIMING
//#define DOUBLE_CHECK_EXACT_OUTPUT
//#define SMALL_CELL_REMOVAL

template <
  typename DerivedVA,
  typename DerivedFA,
  typename DerivedVB,
  typename DerivedFB,
  typename DerivedVC,
  typename DerivedFC,
  typename DerivedJ>
IGL_INLINE bool igl::copyleft::cgal::mesh_boolean(
    const Eigen::MatrixBase<DerivedVA > & VA,
    const Eigen::MatrixBase<DerivedFA > & FA,
    const Eigen::MatrixBase<DerivedVB > & VB,
    const Eigen::MatrixBase<DerivedFB > & FB,
    const MeshBooleanType & type,
    Eigen::PlainObjectBase<DerivedVC > & VC,
    Eigen::PlainObjectBase<DerivedFC > & FC,
    Eigen::PlainObjectBase<DerivedJ > & J)
{
  std::function<int(const int, const int)> keep;
  std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) > wind_num_op;
  mesh_boolean_type_to_funcs(type,wind_num_op,keep);
  return mesh_boolean(VA,FA,VB,FB,wind_num_op,keep,VC,FC,J);
}
template <
  typename DerivedVA,
  typename DerivedFA,
  typename DerivedVB,
  typename DerivedFB,
  typename DerivedVC,
  typename DerivedFC,
  typename DerivedJ>
IGL_INLINE bool igl::copyleft::cgal::mesh_boolean(
  const Eigen::MatrixBase<DerivedVA > & VA,
  const Eigen::MatrixBase<DerivedFA > & FA,
  const Eigen::MatrixBase<DerivedVB > & VB,
  const Eigen::MatrixBase<DerivedFB > & FB,
  const std::string & type_str,
  Eigen::PlainObjectBase<DerivedVC > & VC,
  Eigen::PlainObjectBase<DerivedFC > & FC,
  Eigen::PlainObjectBase<DerivedJ > & J)
{
  return mesh_boolean(
    VA,FA,VB,FB,string_to_mesh_boolean_type(type_str),VC,FC,J);
}

template <
  typename DerivedVA,
  typename DerivedFA,
  typename DerivedVB,
  typename DerivedFB,
  typename DerivedVC,
  typename DerivedFC,
  typename DerivedJ>
IGL_INLINE bool igl::copyleft::cgal::mesh_boolean(
    const Eigen::MatrixBase<DerivedVA> & VA,
    const Eigen::MatrixBase<DerivedFA> & FA,
    const Eigen::MatrixBase<DerivedVB> & VB,
    const Eigen::MatrixBase<DerivedFB> & FB,
    const std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) >& wind_num_op,
    const std::function<int(const int, const int)> & keep,
    Eigen::PlainObjectBase<DerivedVC > & VC,
    Eigen::PlainObjectBase<DerivedFC > & FC,
    Eigen::PlainObjectBase<DerivedJ > & J) 
{
  // Generate combined mesh (VA,FA,VB,FB) -> (V,F)
  Eigen::Matrix<size_t,2,1> sizes(FA.rows(),FB.rows());
  // TODO: This is a precision template **bug** that results in failure to
  // compile. If DerivedVA::Scalar is double and DerivedVB::Scalar is
  // CGAL::Epeck::FT then the following assignment will not compile. This
  // implies that VA must have the trumping precision (and a valid assignment
  // operator from VB's type).
  Eigen::Matrix<typename DerivedVA::Scalar,Eigen::Dynamic,3> VV(VA.rows() + VB.rows(), 3);
  Eigen::Matrix<typename DerivedFA::Scalar,Eigen::Dynamic,3> FF(FA.rows() + FB.rows(), 3);
  // Can't use comma initializer
  for(int a = 0;a<VA.rows();a++)
  {
    for(int d = 0;d<3;d++) VV(a,d) = VA(a,d);
  }
  for(int b = 0;b<VB.rows();b++)
  {
    for(int d = 0;d<3;d++) VV(VA.rows()+b,d) = VB(b,d);
  }
  FF.block(0, 0, FA.rows(), 3) = FA;
  // Eigen struggles to assign nothing to nothing and will assert if FB is empty
  if(FB.rows() > 0)
  {
    FF.block(FA.rows(), 0, FB.rows(), 3) = FB.array() + VA.rows();
  }
  return mesh_boolean(VV,FF,sizes,wind_num_op,keep,VC,FC,J);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedVC,
  typename DerivedFC,
  typename DerivedJ>
IGL_INLINE bool igl::copyleft::cgal::mesh_boolean(
    const std::vector<DerivedV > & Vlist,
    const std::vector<DerivedF > & Flist,
    const std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) >& wind_num_op,
    const std::function<int(const int, const int)> & keep,
    Eigen::PlainObjectBase<DerivedVC > & VC,
    Eigen::PlainObjectBase<DerivedFC > & FC,
    Eigen::PlainObjectBase<DerivedJ > & J)
{
  PlainMatrix<DerivedV,Eigen::Dynamic> VV;
  PlainMatrix<DerivedF,Eigen::Dynamic> FF;
  Eigen::Matrix<size_t,Eigen::Dynamic,1> Vsizes,Fsizes;
  igl::combine(Vlist,Flist,VV,FF,Vsizes,Fsizes);
  return mesh_boolean(VV,FF,Fsizes,wind_num_op,keep,VC,FC,J);
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedVC,
  typename DerivedFC,
  typename DerivedJ>
IGL_INLINE bool igl::copyleft::cgal::mesh_boolean(
    const std::vector<DerivedV > & Vlist,
    const std::vector<DerivedF > & Flist,
    const MeshBooleanType & type,
    Eigen::PlainObjectBase<DerivedVC > & VC,
    Eigen::PlainObjectBase<DerivedFC > & FC,
    Eigen::PlainObjectBase<DerivedJ > & J)
{
  PlainMatrix<DerivedV,Eigen::Dynamic> VV;
  PlainMatrix<DerivedF,Eigen::Dynamic> FF;
  Eigen::Matrix<size_t,Eigen::Dynamic,1> Vsizes,Fsizes;
  igl::combine(Vlist,Flist,VV,FF,Vsizes,Fsizes);
  std::function<int(const int, const int)> keep;
  std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) > wind_num_op;
  mesh_boolean_type_to_funcs(type,wind_num_op,keep);
  return mesh_boolean(VV,FF,Fsizes,wind_num_op,keep,VC,FC,J);
}

template <
  typename DerivedVV,
  typename DerivedFF,
  typename Derivedsizes,
  typename DerivedVC,
  typename DerivedFC,
  typename DerivedJ>
IGL_INLINE bool igl::copyleft::cgal::mesh_boolean(
    const Eigen::MatrixBase<DerivedVV > & VV,
    const Eigen::MatrixBase<DerivedFF > & FF,
    const Eigen::MatrixBase<Derivedsizes> & sizes,
    const std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) >& wind_num_op,
    const std::function<int(const int, const int)> & keep,
    Eigen::PlainObjectBase<DerivedVC > & VC,
    Eigen::PlainObjectBase<DerivedFC > & FC,
    Eigen::PlainObjectBase<DerivedJ > & J)
{
#ifdef MESH_BOOLEAN_TIMING
  const auto & tictoc = []() -> double
  {
    static double t_start = igl::get_seconds();
    double diff = igl::get_seconds()-t_start;
    t_start += diff;
    return diff;
  };
  const auto log_time = [&](const std::string& label) -> void {
    printf("%50s: %0.5lf\n",
      C_STR("mesh_boolean." << label),tictoc());
  };
  tictoc();
#endif
  typedef CGAL::Epeck Kernel;
  typedef Kernel::FT ExactScalar;
  typedef Eigen::Matrix<typename DerivedJ::Scalar,Eigen::Dynamic,1> VectorXJ;
  typedef Eigen::Matrix<
    ExactScalar,
    Eigen::Dynamic,
    Eigen::Dynamic,
    DerivedVC::IsRowMajor> MatrixXES;
  MatrixXES V;
  DerivedFC F;
  VectorXJ  CJ;
  {
    igl::copyleft::cgal::RemeshSelfIntersectionsParam params;
    params.stitch_all = true;
    Eigen::MatrixXi IF;
    remesh_self_intersections(VV,FF,params,V,F,IF,CJ);
  }
  
#ifdef MESH_BOOLEAN_TIMING
  log_time("resolve_self_intersection");
#endif

  Eigen::MatrixXi E, uE;
  Eigen::VectorXi EMAP, uEC, uEE;
  igl::unique_edge_map(F, E, uE, EMAP, uEC, uEE);

  // Compute patches (F,EMAP,uEC,uEE) --> (P)
  Eigen::VectorXi P;
  const size_t num_patches = igl::extract_manifold_patches(F,EMAP,uEC,uEE,P);
#ifdef MESH_BOOLEAN_TIMING
  log_time("patch_extraction");
#endif

  // Compute cells (V,F,P,E,uE,EMAP) -> (per_patch_cells)
  Eigen::MatrixXi per_patch_cells;
  const size_t num_cells =
  extract_cells( V, F, P, uE, EMAP, uEC, uEE, per_patch_cells);
#ifdef MESH_BOOLEAN_TIMING
  log_time("cell_extraction");
#endif

  // Compute winding numbers on each side of each facet.
  const size_t num_faces = F.rows();
  // W(f,:) --> [w1out,w1in,w2out,w2in, ... wnout,wnint] winding numbers above
  // and below each face w.r.t. each input mesh, so that W(f,2*i) is the
  // winding number above face f w.r.t. input i, and W(f,2*i+1) is the winding
  // number below face f w.r.t. input i.
  Eigen::MatrixXi W;
  // labels(f) = i means that face f comes from mesh i
  Eigen::VectorXi labels(num_faces);
  // cumulative sizes
  PlainVector<Derivedsizes,Eigen::Dynamic> cumsizes;
  igl::cumsum(sizes,1,cumsizes);
  const size_t num_inputs = sizes.size();
  std::transform(
    CJ.data(), 
    CJ.data()+CJ.size(), 
    labels.data(),
    // Determine which input mesh birth face i comes from
    [&num_inputs,&cumsizes](int i)->int
    { 
      for(int k = 0;k<num_inputs;k++)
      {
        if(i<cumsizes(k)) return k;
      }
      assert(false && "Birth parent index out of range");
      return -1;
    });
  bool valid = true;
  if (num_faces > 0) 
  {
    valid = valid && propagate_winding_numbers(
      V,F,uE,uEC,uEE,num_patches,P,num_cells,per_patch_cells,labels,W);
  } else 
  {
    W.resize(0, 2*num_inputs);
  }
  assert((size_t)W.rows() == num_faces);
  // If W doesn't have enough columns, pad with zeros
  if (W.cols() <= 2*num_inputs) 
  {
    const int old_ncols = W.cols();
    W.conservativeResize(num_faces,2*num_inputs);
    W.rightCols(2*num_inputs-old_ncols).setConstant(0);
  }
  assert((size_t)W.cols() == 2*num_inputs);
#ifdef MESH_BOOLEAN_TIMING
  log_time("propagate_input_winding_number");
#endif

  // Compute resulting winding number.
  Eigen::MatrixXi Wr(num_faces, 2);
  for (size_t i=0; i<num_faces; i++) 
  {
    // Winding number vectors above and below
    Eigen::RowVectorXi w_out(1,num_inputs), w_in(1,num_inputs);
    for(size_t k =0;k<num_inputs;k++)
    {
      w_out(k) = W(i,2*k+0);
      w_in(k) = W(i,2*k+1);
    }
    Wr(i,0) = wind_num_op(w_out);
    Wr(i,1) = wind_num_op(w_in);
  }
#ifdef MESH_BOOLEAN_TIMING
  log_time("compute_output_winding_number");
#endif

#ifdef SMALL_CELL_REMOVAL
  igl::copyleft::cgal::relabel_small_immersed_cells(
    V, F, num_patches, P, num_cells, per_patch_cells, 1e-3, Wr);
#endif

  // Extract boundary separating inside from outside.
  auto index_to_signed_index = [&](size_t i, bool ori) -> int
  {
    return (i+1)*(ori?1:-1);
  };
  //auto signed_index_to_index = [&](int i) -> size_t {
  //    return abs(i) - 1;
  //};
  std::vector<int> selected;
  for(size_t i=0; i<num_faces; i++) 
  {
    auto should_keep = keep(Wr(i,0), Wr(i,1));
    if (should_keep > 0) 
    {
      selected.push_back(index_to_signed_index(i, true));
    } else if (should_keep < 0) 
    {
      selected.push_back(index_to_signed_index(i, false));
    }
  }

  const size_t num_selected = selected.size();
  DerivedFC kept_faces(num_selected, 3);
  DerivedJ  kept_face_indices(num_selected, 1);
  for (size_t i=0; i<num_selected; i++) 
  {
    size_t idx = abs(selected[i]) - 1;
    if (selected[i] > 0) 
    {
      kept_faces.row(i) = F.row(idx);
    } else 
    {
      kept_faces.row(i) = F.row(idx).reverse();
    }
    kept_face_indices(i, 0) = CJ[idx];
  }
#ifdef MESH_BOOLEAN_TIMING
  log_time("extract_output");
#endif

  // Finally, remove duplicated faces and unreferenced vertices.
  {
    DerivedFC G;
    DerivedJ JJ;
    igl::resolve_duplicated_faces(kept_faces, G, JJ);
    J = kept_face_indices(JJ);

#ifdef DOUBLE_CHECK_EXACT_OUTPUT
    {
      // Sanity check on exact output.
      igl::copyleft::cgal::RemeshSelfIntersectionsParam params;
      params.detect_only = true;
      params.first_only = true;
      MatrixXES dummy_VV;
      DerivedFC dummy_FF, dummy_IF;
      Eigen::VectorXi dummy_J, dummy_IM;
      igl::copyleft::cgal::SelfIntersectMesh<
        Kernel,
        MatrixXES, DerivedFC,
        MatrixXES, DerivedFC,
        DerivedFC,
        Eigen::VectorXi,
        Eigen::VectorXi
      > checker(V, G, params,
          dummy_VV, dummy_FF, dummy_IF, dummy_J, dummy_IM);
      if (checker.count != 0) 
      {
        throw "Self-intersection not fully resolved.";
      }
    }
#endif

    // `assign` _after_ removing unreferenced vertices
    {
      Eigen::VectorXi I,J;
      igl::remove_unreferenced(V.rows(),G,I,J);
      FC = G;
      std::for_each(FC.data(),FC.data()+FC.size(),
          [&I](typename DerivedFC::Scalar & a){a=I(a);});
      const bool slow_and_more_precise = false;
      VC.resize(J.size(),3);
      igl::parallel_for(J.size(),[&](Eigen::Index i)
      {
        for(Eigen::Index c = 0;c<3;c++)
        {
          assign_scalar(V(J(i),c),slow_and_more_precise,VC(i,c));
        }
      },1000);
    }


  }
#ifdef MESH_BOOLEAN_TIMING
  log_time("clean_up");
#endif
  return valid;
}

template <
  typename DerivedVA,
  typename DerivedFA,
  typename DerivedVB,
  typename DerivedFB,
  typename DerivedVC,
  typename DerivedFC>
IGL_INLINE bool igl::copyleft::cgal::mesh_boolean(
  const Eigen::MatrixBase<DerivedVA > & VA,
  const Eigen::MatrixBase<DerivedFA > & FA,
  const Eigen::MatrixBase<DerivedVB > & VB,
  const Eigen::MatrixBase<DerivedFB > & FB,
  const MeshBooleanType & type,
  Eigen::PlainObjectBase<DerivedVC > & VC,
  Eigen::PlainObjectBase<DerivedFC > & FC) 
{
  Eigen::Matrix<typename DerivedFC::Scalar, Eigen::Dynamic,1> J;
  return igl::copyleft::cgal::mesh_boolean(VA,FA,VB,FB,type,VC,FC,J);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
// generated by autoexplicit.sh
template bool igl::copyleft::cgal::mesh_boolean<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::MeshBooleanType const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
// generated by autoexplicit.sh
template bool igl::copyleft::cgal::mesh_boolean<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::MeshBooleanType const&, Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template bool igl::copyleft::cgal::mesh_boolean<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::MeshBooleanType const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template bool igl::copyleft::cgal::mesh_boolean<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::MeshBooleanType const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#ifdef WIN32
#endif
#endif
