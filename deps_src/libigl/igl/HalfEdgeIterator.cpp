// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2014 Daniele Panozzo <daniele.panozzo@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.

#include "HalfEdgeIterator.h"

template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::HalfEdgeIterator(
    const Eigen::PlainObjectBase<DerivedF>& _F,
    const Eigen::PlainObjectBase<DerivedFF>& _FF,
    const Eigen::PlainObjectBase<DerivedFFi>& _FFi,
    int _fi,
    int _ei,
    bool _reverse
)
: fi(_fi), ei(_ei), reverse(_reverse), F(_F), FF(_FF), FFi(_FFi)
{}

template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE void igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::flipF()
{
  if (isBorder())
    return;

  int fin = (FF)(fi,ei);
  int ein = (FFi)(fi,ei);

  fi = fin;
  ei = ein;
  reverse = !reverse;
}


// Change Edge
template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE void igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::flipE()
{
  if (!reverse)
    ei = (ei+2)%3; // ei-1
  else
    ei = (ei+1)%3;

  reverse = !reverse;
}

// Change Vertex
template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE void igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::flipV()
{
  reverse = !reverse;
}

template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE bool igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::isBorder()
{
  return (FF)(fi,ei) == -1;
}

/*!
 * Returns the next edge skipping the border
 *      _________
 *     /\ c | b /\
 *    /  \  |  /  \
 *   / d  \ | / a  \
 *  /______\|/______\
 *          v
 * In this example, if a and d are of-border and the pos is iterating counterclockwise, this method iterate through the faces incident on vertex v,
 * producing the sequence a, b, c, d, a, b, c, ...
 */
template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE bool igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::NextFE()
{
  if ( isBorder() ) // we are on a border
  {
    do
    {
      flipF();
      flipE();
    } while (!isBorder());
    flipE();
    return false;
  }
  else
  {
    flipF();
    flipE();
    return true;
  }
}

// Get vertex index
template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE int igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::Vi()
{
  assert(fi >= 0);
  assert(fi < F.rows());
  assert(ei >= 0);
  assert(ei <= 2);

  if (!reverse)
    return (F)(fi,ei);
  else
    return (F)(fi,(ei+1)%3);
}

// Get face index
template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE int igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::Fi()
{
  return fi;
}

// Get edge index
template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE int igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::Ei()
{
  return ei;
}


template <typename DerivedF, typename DerivedFF, typename DerivedFFi>
IGL_INLINE bool igl::HalfEdgeIterator<DerivedF,DerivedFF,DerivedFFi>::operator==(HalfEdgeIterator& p2)
{
  return
      (
          (fi == p2.fi) &&
              (ei == p2.ei) &&
              (reverse == p2.reverse) &&
              (F   == p2.F) &&
              (FF  == p2.FF) &&
              (FFi == p2.FFi)
      );
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template      igl::HalfEdgeIterator<Eigen::Matrix<int, -1, 3, 0, -1, 3>  ,Eigen::Matrix<int, -1, 3, 0, -1, 3>  ,Eigen::Matrix<int, -1, 3, 0, -1, 3>   >::HalfEdgeIterator(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, int, int, bool);
template igl::HalfEdgeIterator<Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >::HalfEdgeIterator(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, int, bool);
template bool igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::NextFE();
template int  igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::Ei();
template int  igl::HalfEdgeIterator<Eigen::Matrix<int, -1, 3, 0, -1, 3>  ,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::Ei();
template int  igl::HalfEdgeIterator<Eigen::Matrix<int, -1, 3, 0, -1, 3>  ,Eigen::Matrix<int, -1, 3, 0, -1, 3>  ,Eigen::Matrix<int, -1, 3, 0, -1, 3>   >::Ei();
template int  igl::HalfEdgeIterator<Eigen::Matrix<int, -1, 3, 0, -1, 3>  ,Eigen::Matrix<int, -1, 3, 0, -1, 3>  ,Eigen::Matrix<int, -1, 3, 0, -1, 3>   >::Fi();
template bool igl::HalfEdgeIterator<Eigen::Matrix<int, -1, 3, 0, -1, 3>  ,Eigen::Matrix<int, -1, 3, 0, -1, 3>  ,Eigen::Matrix<int, -1, 3, 0, -1, 3>   >::NextFE();
template int  igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::Vi();
template      igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::HalfEdgeIterator(Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, int, int, bool);
template int  igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::Fi();
template void igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::flipE();
template void igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::flipF();
template void igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::flipV();
template bool igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >::operator==(igl::HalfEdgeIterator<Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1>,Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template int igl::HalfEdgeIterator<Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >::Fi();
template bool igl::HalfEdgeIterator<Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >::NextFE();
#endif
