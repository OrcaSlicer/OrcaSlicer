// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2014 Alec Jacobson <alecjacobson@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "intersect_other.h"
#include "CGAL_includes.hpp"
#include "mesh_to_cgal_triangle_list.h"
#include "remesh_intersections.h"
#include "../../remove_unreferenced.h"
#include "../../PlainMatrix.h"

#ifndef IGL_FIRST_HIT_EXCEPTION
#define IGL_FIRST_HIT_EXCEPTION 10
#endif

// Un-exposed helper functions
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      template <typename Index>
      static IGL_INLINE void push_result(
        const int f,
        const int f_other,
        const CGAL::Object & result,
        std::map<
          Index,
          std::vector<std::pair<Index, CGAL::Object> > > &
          offending)
        //std::map<
        //  std::pair<typename DerivedF::Index,typename DerivedF::Index>,
        //  std::vector<typename DerivedF::Index> > & edge2faces)
      {
        if(offending.count(f) == 0)
        {
          // first time marking, initialize with new id and empty list
          offending[f] = {};
        }
        offending[f].push_back({f_other,result});
      }
      template <
        typename Kernel,
        typename DerivedVA,
        typename DerivedFA,
        typename DerivedVB,
        typename DerivedFB,
        typename DerivedIF,
        typename DerivedVVAB,
        typename DerivedFFAB,
        typename DerivedJAB,
        typename DerivedIMAB>
      static IGL_INLINE bool intersect_other_helper(
        const Eigen::MatrixBase<DerivedVA> & VA,
        const Eigen::MatrixBase<DerivedFA> & FA,
        const Eigen::MatrixBase<DerivedVB> & VB,
        const Eigen::MatrixBase<DerivedFB> & FB,
        const RemeshSelfIntersectionsParam & params,
        Eigen::PlainObjectBase<DerivedIF> & IF,
        Eigen::PlainObjectBase<DerivedVVAB> & VVAB,
        Eigen::PlainObjectBase<DerivedFFAB> & FFAB,
        Eigen::PlainObjectBase<DerivedJAB>  & JAB,
        Eigen::PlainObjectBase<DerivedIMAB> & IMAB)
      {

        using namespace std;
        using namespace Eigen;

        typedef typename DerivedFA::Index Index;
        typedef CGAL::Triangle_3<Kernel> Triangle_3; 
        //// Axis-align boxes for all-pairs self-intersection detection
        typedef std::vector<Triangle_3> Triangles;
        typedef typename Triangles::iterator TrianglesIterator;
        typedef 
          CGAL::Box_intersection_d::Box_with_handle_d<double,3,TrianglesIterator> 
          Box;
        typedef 
          std::map<Index,std::vector<std::pair<Index,CGAL::Object> > > 
          OffendingMap;

        Triangles TA,TB;
        // Compute and process self intersections
        mesh_to_cgal_triangle_list(VA,FA,TA);
        mesh_to_cgal_triangle_list(VB,FB,TB);
        // http://www.cgal.org/Manual/latest/doc_html/cgal_manual/Box_intersection_d/Chapter_main.html#Section_63.5 
        // Create the corresponding vector of bounding boxes
        std::vector<Box> A_boxes,B_boxes;
        const auto box_up = [](Triangles & T, std::vector<Box> & boxes) -> void
        {
          boxes.reserve(T.size());
          for ( 
            TrianglesIterator tit = T.begin(); 
            tit != T.end(); 
            ++tit)
          {
            boxes.push_back(Box(tit->bbox(), tit));
          }
        };
        box_up(TA,A_boxes);
        box_up(TB,B_boxes);
        OffendingMap offendingA,offendingB;
        //EdgeMap edge2facesA,edge2facesB;

        std::list<int> lIF;
        const auto cb = [&](const Box &a, const Box &b) -> void
        {
          using namespace std;
          // index in F and T
          int fa = a.handle()-TA.begin();
          int fb = b.handle()-TB.begin();
          const Triangle_3 & A = *a.handle();
          const Triangle_3 & B = *b.handle();
          if(CGAL::do_intersect(A,B))
          {
            // There was an intersection
            lIF.push_back(fa);
            lIF.push_back(fb);
            if(params.first_only)
            {
              throw IGL_FIRST_HIT_EXCEPTION;
            }
            if(!params.detect_only)
            {
              CGAL::Object result = CGAL::intersection(A,B);

              push_result(fa,fb,result,offendingA);
              push_result(fb,fa,result,offendingB);
            }
          }
        };
        try{
          CGAL::box_intersection_d(
            A_boxes.begin(), A_boxes.end(),
            B_boxes.begin(), B_boxes.end(),
            cb,
            std::ptrdiff_t(params.cutoff));
        }catch(int e)
        {
          // Rethrow if not FIRST_HIT_EXCEPTION
          if(e != IGL_FIRST_HIT_EXCEPTION)
          {
            throw e;
          }
          // Otherwise just fall through
        }

        // Convert lIF to Eigen matrix
        assert(lIF.size()%2 == 0);
        IF.resize(lIF.size()/2,2);
        {
          int i=0;
          for(
            list<int>::const_iterator ifit = lIF.begin();
            ifit!=lIF.end();
            )
          {
            IF(i,0) = (*ifit);
            ifit++; 
            IF(i,1) = (*ifit);
            ifit++;
            i++;
          }
        }
        if(!params.detect_only)
        {
          // Obsolete, now remesh_intersections expects a single mesh
          // remesh_intersections(VA,FA,TA,offendingA,VVA,FFA,JA,IMA);
          // remesh_intersections(VB,FB,TB,offendingB,VVB,FFB,JB,IMB);
          // Combine mesh and offending maps
          PlainMatrix<DerivedVA,Eigen::Dynamic> VAB(VA.rows()+VB.rows(),3);
          VAB<<VA,VB;
          PlainMatrix<DerivedFA,Eigen::Dynamic> FAB(FA.rows()+FB.rows(),3);
          FAB<<FA,(FB.array()+VA.rows());
          Triangles TAB;
          TAB.reserve(TA.size()+TB.size());
          TAB.insert(TAB.end(),TA.begin(),TA.end());
          TAB.insert(TAB.end(),TB.begin(),TB.end());
          OffendingMap offending;
          //offending.reserve(offendingA.size() + offendingB.size());
          for (const auto itr : offendingA)
          {
            // Remap offenders in FB to FAB
            auto offenders = itr.second;
            for(auto & offender : offenders)
            {
              offender.first += FA.rows();
            }
            offending[itr.first] = offenders;
          }
          for (const auto itr : offendingB)
          {
            // Store offenders for FB according to place in FAB
            offending[FA.rows() + itr.first] = itr.second;
          }

          remesh_intersections(
            VAB,FAB,TAB,offending,
            params.stitch_all,params.slow_and_more_precise_rounding,
            VVAB,FFAB,JAB,IMAB);
        }

        return IF.rows() > 0;
      }
    }
  }
}

template <
  typename DerivedVA,
  typename DerivedFA,
  typename DerivedVB,
  typename DerivedFB,
  typename DerivedIF,
  typename DerivedVVAB,
  typename DerivedFFAB,
  typename DerivedJAB,
  typename DerivedIMAB>
IGL_INLINE bool igl::copyleft::cgal::intersect_other(
    const Eigen::MatrixBase<DerivedVA> & VA,
    const Eigen::MatrixBase<DerivedFA> & FA,
    const Eigen::MatrixBase<DerivedVB> & VB,
    const Eigen::MatrixBase<DerivedFB> & FB,
    const RemeshSelfIntersectionsParam & params,
    Eigen::PlainObjectBase<DerivedIF> & IF,
    Eigen::PlainObjectBase<DerivedVVAB> & VVAB,
    Eigen::PlainObjectBase<DerivedFFAB> & FFAB,
    Eigen::PlainObjectBase<DerivedJAB>  & JAB,
    Eigen::PlainObjectBase<DerivedIMAB> & IMAB)
{
  typedef typename DerivedVA::Scalar VAScalar;
  typedef typename DerivedVB::Scalar VBScalar;
  if(params.detect_only && ! (
    std::is_same<VAScalar,CGAL::Epeck::FT>::value ||
    std::is_same<VBScalar,CGAL::Epeck::FT>::value
    ))
  {
    return intersect_other_helper<CGAL::Epick>
      (VA,FA,VB,FB,params,IF,VVAB,FFAB,JAB,IMAB);
  }else
  {
    return intersect_other_helper<CGAL::Epeck>
      (VA,FA,VB,FB,params,IF,VVAB,FFAB,JAB,IMAB);
  }
}

template <
  typename DerivedVA,
  typename DerivedFA,
  typename DerivedVB,
  typename DerivedFB,
  typename DerivedIF>
IGL_INLINE bool igl::copyleft::cgal::intersect_other(
  const Eigen::MatrixBase<DerivedVA> & VA,
  const Eigen::MatrixBase<DerivedFA> & FA,
  const Eigen::MatrixBase<DerivedVB> & VB,
  const Eigen::MatrixBase<DerivedFB> & FB,
  const bool first_only,
  Eigen::PlainObjectBase<DerivedIF> & IF)
{
  PlainMatrix<DerivedVA,Eigen::Dynamic> VVAB;
  PlainMatrix<DerivedFA,Eigen::Dynamic> FFAB;
  Eigen::VectorXi JAB, IMAB;
  return intersect_other(
    VA,FA,VB,FB,{true,first_only},IF,VVAB,FFAB,JAB,IMAB);
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::copyleft::cgal::intersect_other<Eigen::Matrix<double, -1, -1, 0, -1, -1>,   Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>,   Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>,   Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1,   -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(
Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&,   
Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&,   
Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&,   
Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&,   igl::copyleft::cgal::RemeshSelfIntersectionsParam const&,   
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&,   
Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, 3, 0, -1, 3> >&,   
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&,   
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&,   
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
template bool igl::copyleft::cgal::intersect_other<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, 1, 0, -1, 1> >(
Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, 
Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, 
Eigen::MatrixBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> > const&, 
Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, igl::copyleft::cgal::RemeshSelfIntersectionsParam const&, 
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, 
Eigen::PlainObjectBase<Eigen::Matrix<CGAL::Epeck::FT, -1, -1, 1, -1, -1> >&, 
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, 
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, 
Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&);
#endif
