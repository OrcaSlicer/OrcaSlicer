/*******************************************************************************
*                                                                              *
* Author    :  Angus Johnson                                                   *
* Version   :  6.4.2                                                           *
* Date      :  27 February 2017                                                *
* Website   :  http://www.angusj.com                                           *
* Copyright :  Angus Johnson 2010-2017                                         *
*                                                                              *
* License:                                                                     *
* Use, modification & distribution is subject to Boost Software License Ver 1. *
* http://www.boost.org/LICENSE_1_0.txt                                         *
*                                                                              *
* Attributions:                                                                *
* The code in this library is an extension of Bala Vatti's clipping algorithm: *
* "A generic solution to polygon clipping"                                     *
* Communications of the ACM, Vol 35, Issue 7 (July 1992) pp 56-63.             *
* http://portal.acm.org/citation.cfm?id=129906                                 *
*                                                                              *
* Computer graphics and geometric modeling: implementation and algorithms      *
* By Max K. Agoston                                                            *
* Springer; 1 edition (January 4, 2005)                                        *
* http://books.google.com/books?q=vatti+clipping+agoston                       *
*                                                                              *
* See also:                                                                    *
* "Polygon Offsetting by Computing Winding Numbers"                            *
* Paper no. DETC2005-85513 pp. 565-575                                         *
* ASME 2005 International Design Engineering Technical Conferences             *
* and Computers and Information in Engineering Conference (IDETC/CIE2005)      *
* September 24-28, 2005 , Long Beach, California, USA                          *
* http://www.me.berkeley.edu/~mcmains/pubs/DAC05OffsetPolygon.pdf              *
*                                                                              *
*******************************************************************************/

/*******************************************************************************
*                                                                              *
* This is a translation of the Delphi Clipper library and the naming style     *
* used has retained a Delphi flavour.                                          *
*                                                                              *
*******************************************************************************/

#include "clipper.hpp"
#include <cmath>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cstdlib>
#include <ostream>
#include <functional>
#include <assert.h>
#include <libslic3r/Int128.hpp>

#ifdef CLIPPERLIB_NAMESPACE_PREFIX
namespace CLIPPERLIB_NAMESPACE_PREFIX {
#endif // CLIPPERLIB_NAMESPACE_PREFIX

#ifdef CLIPPERLIB_USE_XYZ
namespace ClipperLib_Z {
#else /* CLIPPERLIB_USE_XYZ */
namespace ClipperLib {
#endif /* CLIPPERLIB_USE_XYZ */

static double const pi = 3.141592653589793238;
static double const two_pi = pi *2;
static double const def_arc_tolerance = 0.25;

enum Direction { dRightToLeft, dLeftToRight };

static int const Unassigned = -1;  //edge not currently 'owning' a solution
static int const Skip = -2;        //edge that would otherwise close a path

#define HORIZONTAL (-1.0E+40)
#define TOLERANCE (1.0e-20)
#define NEAR_ZERO(val) (((val) > -TOLERANCE) && ((val) < TOLERANCE))

//------------------------------------------------------------------------------

inline IntPoint IntPoint2d(cInt x, cInt y)
{
  return IntPoint(x, y
#ifdef CLIPPERLIB_USE_XYZ
    , 0
#endif // CLIPPERLIB_USE_XYZ
    );
}

// Fast rounding upwards.
inline double FRound(double a)
{
    // Why does Java Math.round(0.49999999999999994) return 1?
    // https://stackoverflow.com/questions/9902968/why-does-math-round0-49999999999999994-return-1
    return a == 0.49999999999999994 ? 0 : floor(a + 0.5);
}

template<typename IType>
inline IType Round(double val)
{
    double v = FRound(val);
#if defined(CLIPPERLIB_INT32) && ! defined(NDEBUG)
    static_assert(sizeof(IType) == 4 || sizeof(IType) == 8, "IType must be int32 or int64");
    static constexpr const double hi = 65536. * 16383. * (sizeof(IType) == 4 ? 1 : 65536. * 65536.);
    if (v > hi || -v > hi)
        throw clipperException("Coordinate outside allowed range");
#endif
    return static_cast<IType>(v);
}

// Overriding the Eigen operators because we don't want to compare Z coordinate if IntPoint is 3 dimensional.
inline bool operator==(const IntPoint &l, const IntPoint &r) { return l.x() == r.x() && l.y() == r.y(); }
inline bool operator!=(const IntPoint &l, const IntPoint &r) { return l.x() != r.x() || l.y() != r.y(); }

//------------------------------------------------------------------------------
// PolyTree methods ...
//------------------------------------------------------------------------------

int PolyTree::Total() const
{
  int result = (int)AllNodes.size();
  //with negative offsets, ignore the hidden outer polygon ...
  if (result > 0 && Childs.front() != &AllNodes.front()) result--;
  return result;
}

//------------------------------------------------------------------------------
// PolyNode methods ...
//------------------------------------------------------------------------------

void PolyNode::AddChild(PolyNode& child)
{
  unsigned cnt = (unsigned)Childs.size();
  Childs.emplace_back(&child);
  child.Parent = this;
  child.Index = cnt;
}
//------------------------------------------------------------------------------

// Edge delimits a hole if it has an odd number of parent loops.
bool PolyNode::IsHole() const
{ 
  bool result = true;
  PolyNode* node = Parent;
  while (node)
  {
      result = !result;
      node = node->Parent;
  }
  return result;
}

void PolyTree::RemoveOutermostPolygon()
{
    if (this->ChildCount() == 1 && this->Childs[0]->ChildCount() > 0) {
        PolyNode *outerNode = this->Childs[0];
        this->Childs.reserve(outerNode->ChildCount());
        this->Childs[0] = outerNode->Childs[0];
        this->Childs[0]->Parent = outerNode->Parent;
        for (int i = 1; i < outerNode->ChildCount(); ++i)
            this->AddChild(*outerNode->Childs[i]);
    } else
        this->Clear();
}

//------------------------------------------------------------------------------
// Miscellaneous global functions
//------------------------------------------------------------------------------

double Area(const Path &poly)
{
  int size = (int)poly.size();
  if (size < 3) return 0;

  double a = 0;
  for (int i = 0, j = size -1; i < size; ++i)
  {
    a += ((double)poly[j].x() + poly[i].x()) * ((double)poly[j].y() - poly[i].y());
    j = i;
  }
  return -a * 0.5;
}
//------------------------------------------------------------------------------

double Area(const OutPt *op)
{
  const OutPt *startOp = op;
  if (!op) return 0;
  double a = 0;
  do {
    a +=  (double)(op->Prev->Pt.x() + op->Pt.x()) * (double)(op->Prev->Pt.y() - op->Pt.y());
    op = op->Next;
  } while (op != startOp);
  return a * 0.5;
}
//------------------------------------------------------------------------------

double Area(const OutRec &outRec)
{
  return Area(outRec.Pts);
}
//------------------------------------------------------------------------------

bool PointIsVertex(const IntPoint &Pt, OutPt *pp)
{
  OutPt *pp2 = pp;
  do
  {
    if (pp2->Pt == Pt) return true;
    pp2 = pp2->Next;
  }
  while (pp2 != pp);
  return false;
}
//------------------------------------------------------------------------------

//See "The Point in Polygon Problem for Arbitrary Polygons" by Hormann & Agathos
//http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.88.5498&rep=rep1&type=pdf
int PointInPolygon(const IntPoint &pt, const Path &path)
{
  //returns 0 if false, +1 if true, -1 if pt ON polygon boundary
  int result = 0;
  size_t cnt = path.size();
  if (cnt < 3) return 0;
  IntPoint ip = path[0];
  for(size_t i = 1; i <= cnt; ++i)
  {
    IntPoint ipNext = (i == cnt ? path[0] : path[i]);
    if (ipNext.y() == pt.y() && ((ipNext.x() == pt.x()) || (ip.y() == pt.y() && ((ipNext.x() > pt.x()) == (ip.x() < pt.x())))))
      return -1;
    if ((ip.y() < pt.y()) != (ipNext.y() < pt.y()))
    {
      if (ip.x() >= pt.x())
      {
        if (ipNext.x() > pt.x()) result = 1 - result;
        else
        {
          auto d = CrossProductType(ip.x() - pt.x()) * CrossProductType(ipNext.y() - pt.y()) - CrossProductType(ipNext.x() - pt.x()) * CrossProductType(ip.y() - pt.y());
          if (!d) return -1;
          if ((d > 0) == (ipNext.y() > ip.y())) result = 1 - result;
        }
      } else
      {
        if (ipNext.x() > pt.x())
        {
          auto d = CrossProductType(ip.x() - pt.x()) * CrossProductType(ipNext.y() - pt.y()) - CrossProductType(ipNext.x() - pt.x()) * CrossProductType(ip.y() - pt.y());
          if (!d) return -1;
          if ((d > 0) == (ipNext.y() > ip.y())) result = 1 - result;
        }
      }
    }
    ip = ipNext;
  } 
  return result;
}
//------------------------------------------------------------------------------

// Called by Poly2ContainsPoly1()
int PointInPolygon(const IntPoint &pt, OutPt *op)
{
  //returns 0 if false, +1 if true, -1 if pt ON polygon boundary
  int result = 0;
  OutPt* startOp = op;
  do
  {
    if (op->Next->Pt.y() == pt.y())
    {
        if ((op->Next->Pt.x() == pt.x()) || (op->Pt.y() == pt.y() && 
          ((op->Next->Pt.x() > pt.x()) == (op->Pt.x() < pt.x())))) return -1;
    }
    if ((op->Pt.y() < pt.y()) != (op->Next->Pt.y() < pt.y()))
    {
      if (op->Pt.x() >= pt.x())
      {
        if (op->Next->Pt.x() > pt.x()) result = 1 - result;
        else
        {
          auto d = CrossProductType(op->Pt.x() - pt.x()) * CrossProductType(op->Next->Pt.y() - pt.y()) - CrossProductType(op->Next->Pt.x() - pt.x()) * CrossProductType(op->Pt.y() - pt.y());
          if (!d) return -1;
          if ((d > 0) == (op->Next->Pt.y() > op->Pt.y())) result = 1 - result;
        }
      } else
      {
        if (op->Next->Pt.x() > pt.x())
        {
          auto d = CrossProductType(op->Pt.x() - pt.x()) * CrossProductType(op->Next->Pt.y() - pt.y()) - CrossProductType(op->Next->Pt.x() - pt.x()) * CrossProductType(op->Pt.y() - pt.y());
          if (!d) return -1;
          if ((d > 0) == (op->Next->Pt.y() > op->Pt.y())) result = 1 - result;
        }
      }
    } 
    op = op->Next;
  } while (startOp != op);
  return result;
}
//------------------------------------------------------------------------------

// This is potentially very expensive! O(n^2)!
bool Poly2ContainsPoly1(OutPt *OutPt1, OutPt *OutPt2)
{
  OutPt* op = OutPt1;
  do
  {
    //nb: PointInPolygon returns 0 if false, +1 if true, -1 if pt on polygon
    int res = PointInPolygon(op->Pt, OutPt2);
    if (res >= 0) return res > 0;
    op = op->Next; 
  }
  while (op != OutPt1);
  return true; 
}
//----------------------------------------------------------------------

#ifdef CLIPPERLIB_INT32
inline bool SlopesEqual(const cInt dx1, const cInt dy1, const cInt dx2, const cInt dy2, bool /* UseFullInt64Range */) {
  return int64_t(dy1) * int64_t(dx2) == int64_t(dx1) * int64_t(dy2);
}
#else
inline bool SlopesEqual(const cInt dx1, const cInt dy1, const cInt dx2, const cInt dy2, bool UseFullInt64Range) {
  return (UseFullInt64Range) ?
    // |dx1| < 2^63, |dx2| < 2^63 etc,
    Int128::sign_determinant_2x2_filtered(dx1, dy1, dx2, dy2) == 0 :
//    Int128::sign_determinant_2x2(dx1, dy1, dx2, dy2) == 0 :
    // |dx1| < 2^31, |dx2| < 2^31 etc,
    // therefore the following computation could be done with 64bit arithmetics. 
    dy1 * dx2 == dx1 * dy2;
}
#endif

inline bool SlopesEqual(const TEdge &e1, const TEdge &e2, bool UseFullInt64Range)
  { return SlopesEqual(e1.Top.x() - e1.Bot.x(), e1.Top.y() - e1.Bot.y(), e2.Top.x() - e2.Bot.x(), e2.Top.y() - e2.Bot.y(), UseFullInt64Range); }
inline bool SlopesEqual(const IntPoint &pt1, const IntPoint &pt2, const IntPoint &pt3, bool UseFullInt64Range)
  { return SlopesEqual(pt1.x()-pt2.x(), pt1.y()-pt2.y(), pt2.x()-pt3.x(), pt2.y()-pt3.y(), UseFullInt64Range); }
inline bool SlopesEqual(const IntPoint &pt1, const IntPoint &pt2, const IntPoint &pt3, const IntPoint &pt4, bool UseFullInt64Range)
  { return SlopesEqual(pt1.x()-pt2.x(), pt1.y()-pt2.y(), pt3.x()-pt4.x(), pt3.y()-pt4.y(), UseFullInt64Range); }

//------------------------------------------------------------------------------

inline bool IsHorizontal(TEdge &e)
{
  return e.Dx == HORIZONTAL;
}
//------------------------------------------------------------------------------

inline double GetDx(const IntPoint &pt1, const IntPoint &pt2)
{
  return (pt1.y() == pt2.y()) ?
    HORIZONTAL : (double)(pt2.x() - pt1.x()) / (pt2.y() - pt1.y());
}
//---------------------------------------------------------------------------

inline cInt TopX(TEdge &edge, const cInt currentY)
{
  return (currentY == edge.Top.y()) ?
    edge.Top.x() : 
    edge.Bot.x() + Round<cInt>(edge.Dx *(currentY - edge.Bot.y()));
}
//------------------------------------------------------------------------------

void IntersectPoint(TEdge &Edge1, TEdge &Edge2, IntPoint &ip)
{
#ifdef CLIPPERLIB_USE_XYZ  
  ip.z() = 0;
#endif

  if (Edge1.Dx == Edge2.Dx)
  {
    ip.y() = Edge1.Curr.y();
    ip.x() = TopX(Edge1, ip.y());
    return;
  }
 
  int64_t y;
  if (Edge1.Dx == 0)
  {
    ip.x() = Edge1.Bot.x();
    y = IsHorizontal(Edge2) ?
      Edge2.Bot.y() :
      Round<int64_t>(ip.x() / Edge2.Dx + Edge2.Bot.y() - (Edge2.Bot.x() / Edge2.Dx));
      
  }
  else if (Edge2.Dx == 0)
  {
    ip.x() = Edge2.Bot.x();
    y = IsHorizontal(Edge1) ? 
      Edge1.Bot.y() :
      Round<int64_t>(ip.x() / Edge1.Dx + Edge1.Bot.y() - (Edge1.Bot.x() / Edge1.Dx));
  }
  else
  {
    double b1 = double(Edge1.Bot.x()) - double(Edge1.Bot.y()) * Edge1.Dx;
    double b2 = double(Edge2.Bot.x()) - double(Edge2.Bot.y()) * Edge2.Dx;
    double q = (b2-b1) / (Edge1.Dx - Edge2.Dx);
    y = Round<int64_t>(q);
    ip.x() = (std::fabs(Edge1.Dx) < std::fabs(Edge2.Dx)) ?
      Round<cInt>(Edge1.Dx * q + b1) :
      Round<cInt>(Edge2.Dx * q + b2);
  }

  ip.y() = cInt(y);
  if (y < Edge1.Top.y() || y < Edge2.Top.y())
  {
    ip.y() = (Edge1.Top.y() > Edge2.Top.y() ? Edge1 : Edge2).Top.y();
    y = ip.y();
    ip.x() = TopX(std::fabs(Edge1.Dx) < std::fabs(Edge2.Dx) ? Edge1 : Edge2, ip.y());
  }
  //finally, don't allow 'ip' to be BELOW curr.y() (ie bottom of scanbeam) ...
  if (y > Edge1.Curr.y())
  {
    ip.y() = Edge1.Curr.y();
    //use the more vertical edge to derive X ...
    ip.x() = TopX(std::fabs(Edge1.Dx) > std::fabs(Edge2.Dx) ? Edge2 : Edge1, ip.y());
  }
}
//------------------------------------------------------------------------------

// Reverse a linked loop of points representing a closed polygon.
// This has a time complexity of O(n)
void ReversePolyPtLinks(OutPt *pp)
{
  if (!pp) return;
  OutPt *pp1 = pp;
  do {
    OutPt *pp2 = pp1->Next;
    pp1->Next = pp1->Prev;
    pp1->Prev = pp2;
    pp1 = pp2;
  } while( pp1 != pp );
}
//------------------------------------------------------------------------------

inline void InitEdge(TEdge* e, TEdge* eNext, TEdge* ePrev, const IntPoint& Pt)
{
  std::memset(e, 0, sizeof(TEdge));
  e->Next = eNext;
  e->Prev = ePrev;
  e->Curr = Pt;
  e->OutIdx = Unassigned;
}
//------------------------------------------------------------------------------

void InitEdge2(TEdge& e, PolyType Pt)
{
  if (e.Curr.y() >= e.Next->Curr.y())
  {
    e.Bot = e.Curr;
    e.Top = e.Next->Curr;
  } else
  {
    e.Top = e.Curr;
    e.Bot = e.Next->Curr;
  }

  cInt dy = (e.Top.y() - e.Bot.y());
  if (dy == 0) e.Dx = HORIZONTAL;
  else e.Dx = (double)(e.Top.x() - e.Bot.x()) / dy;

  e.PolyTyp = Pt;
}
//------------------------------------------------------------------------------

// Called from ClipperBase::AddPathInternal() to remove collinear and duplicate points.
inline TEdge* RemoveEdge(TEdge* e)
{
  //removes e from double_linked_list (but without removing from memory)
  e->Prev->Next = e->Next;
  e->Next->Prev = e->Prev;
  TEdge* result = e->Next;
  e->Prev = 0; //flag as removed (see ClipperBase.Clear)
  return result;
}
//------------------------------------------------------------------------------

inline void ReverseHorizontal(TEdge &e)
{
  //swap horizontal edges' Top and Bottom x's so they follow the natural
  //progression of the bounds - ie so their xbots will align with the
  //adjoining lower edge. [Helpful in the ProcessHorizontal() method.]
  std::swap(e.Top.x(), e.Bot.x());
#ifdef CLIPPERLIB_USE_XYZ  
  std::swap(e.Top.z(), e.Bot.z());
#endif
}
//------------------------------------------------------------------------------

bool GetOverlapSegment(IntPoint pt1a, IntPoint pt1b, IntPoint pt2a,
  IntPoint pt2b, IntPoint &pt1, IntPoint &pt2)
{
  //precondition: segments are Collinear.
  if (std::abs(pt1a.x() - pt1b.x()) > std::abs(pt1a.y() - pt1b.y()))
  {
    if (pt1a.x() > pt1b.x()) std::swap(pt1a, pt1b);
    if (pt2a.x() > pt2b.x()) std::swap(pt2a, pt2b);
    if (pt1a.x() > pt2a.x()) pt1 = pt1a; else pt1 = pt2a;
    if (pt1b.x() < pt2b.x()) pt2 = pt1b; else pt2 = pt2b;
    return pt1.x() < pt2.x();
  } else
  {
    if (pt1a.y() < pt1b.y()) std::swap(pt1a, pt1b);
    if (pt2a.y() < pt2b.y()) std::swap(pt2a, pt2b);
    if (pt1a.y() < pt2a.y()) pt1 = pt1a; else pt1 = pt2a;
    if (pt1b.y() > pt2b.y()) pt2 = pt1b; else pt2 = pt2b;
    return pt1.y() > pt2.y();
  }
}
//------------------------------------------------------------------------------

bool FirstIsBottomPt(const OutPt* btmPt1, const OutPt* btmPt2)
{
  OutPt *p = btmPt1->Prev;
  while ((p->Pt == btmPt1->Pt) && (p != btmPt1)) p = p->Prev;
  double dx1p = std::fabs(GetDx(btmPt1->Pt, p->Pt));
  p = btmPt1->Next;
  while ((p->Pt == btmPt1->Pt) && (p != btmPt1)) p = p->Next;
  double dx1n = std::fabs(GetDx(btmPt1->Pt, p->Pt));

  p = btmPt2->Prev;
  while ((p->Pt == btmPt2->Pt) && (p != btmPt2)) p = p->Prev;
  double dx2p = std::fabs(GetDx(btmPt2->Pt, p->Pt));
  p = btmPt2->Next;
  while ((p->Pt == btmPt2->Pt) && (p != btmPt2)) p = p->Next;
  double dx2n = std::fabs(GetDx(btmPt2->Pt, p->Pt));

  if (std::max(dx1p, dx1n) == std::max(dx2p, dx2n) &&
    std::min(dx1p, dx1n) == std::min(dx2p, dx2n))
      return Area(btmPt1) > 0; //if otherwise identical use orientation
  else
    return (dx1p >= dx2p && dx1p >= dx2n) || (dx1n >= dx2p && dx1n >= dx2n);
}
//------------------------------------------------------------------------------

// Called by GetLowermostRec()
OutPt* GetBottomPt(OutPt *pp)
{
  OutPt* dups = nullptr;
  OutPt* p = pp->Next;
  while (p != pp)
  {
    if (p->Pt.y() > pp->Pt.y())
    {
      pp = p;
      dups = nullptr;
    }
    else if (p->Pt.y() == pp->Pt.y() && p->Pt.x() <= pp->Pt.x())
    {
      if (p->Pt.x() < pp->Pt.x())
      {
        dups = nullptr;
        pp = p;
      } else
      {
        if (p->Next != pp && p->Prev != pp) dups = p;
      }
    }
    p = p->Next;
  }
  if (dups)
  {
    //there appears to be at least 2 vertices at BottomPt so ...
    while (dups != p)
    {
      if (!FirstIsBottomPt(p, dups)) pp = dups;
      dups = dups->Next;
      while (dups->Pt != pp->Pt) dups = dups->Next;
    }
  }
  return pp;
}

//------------------------------------------------------------------------------

bool Pt2IsBetweenPt1AndPt3(const IntPoint &pt1,
  const IntPoint &pt2, const IntPoint &pt3)
{
  if ((pt1 == pt3) || (pt1 == pt2) || (pt3 == pt2))
    return false;
  else if (pt1.x() != pt3.x())
    return (pt2.x() > pt1.x()) == (pt2.x() < pt3.x());
  else
    return (pt2.y() > pt1.y()) == (pt2.y() < pt3.y());
}
//------------------------------------------------------------------------------

bool HorzSegmentsOverlap(cInt seg1a, cInt seg1b, cInt seg2a, cInt seg2b)
{
  if (seg1a > seg1b) std::swap(seg1a, seg1b);
  if (seg2a > seg2b) std::swap(seg2a, seg2b);
  return (seg1a < seg2b) && (seg2a < seg1b);
}

//------------------------------------------------------------------------------
// ClipperBase class methods ...
//------------------------------------------------------------------------------

#ifdef CLIPPERLIB_INT32
static inline void RangeTest(const IntPoint &pt)
{
#ifndef NDEBUG
    static constexpr const int32_t hi = 65536 * 16383;
    if (pt.x() > hi || pt.y() > hi || -pt.x() > hi || -pt.y() > hi) 
      throw clipperException("Coordinate outside allowed range");
#endif // NDEBUG
}
#else // CLIPPERLIB_INT32
// Called from ClipperBase::AddPath() to verify the scale of the input polygon coordinates.
static inline void RangeTest(const IntPoint& Pt, bool& useFullRange)
{
  if (useFullRange)
  {
    if (Pt.x() > hiRange || Pt.y() > hiRange || -Pt.x() > hiRange || -Pt.y() > hiRange) 
      throw clipperException("Coordinate outside allowed range");
  }
  else if (Pt.x() > loRange|| Pt.y() > loRange || -Pt.x() > loRange || -Pt.y() > loRange) 
  {
    useFullRange = true;
    RangeTest(Pt, useFullRange);
  }
}
#endif // CLIPPERLIB_INT32
//------------------------------------------------------------------------------

// Called from ClipperBase::AddPath() to construct the Local Minima List.
// Find a local minimum edge on the path starting with E.
inline TEdge* FindNextLocMin(TEdge* E)
{
  for (;;)
  {
    while (E->Bot != E->Prev->Bot || E->Curr == E->Top) E = E->Next;
    if (!IsHorizontal(*E) && !IsHorizontal(*E->Prev)) break;
    while (IsHorizontal(*E->Prev)) E = E->Prev;
    TEdge* E2 = E;
    while (IsHorizontal(*E)) E = E->Next;
    if (E->Top.y() == E->Prev->Bot.y()) continue; //ie just an intermediate horz.
    if (E2->Prev->Bot.x() < E->Bot.x()) E = E2;
    break;
  }
  return E;
}
//------------------------------------------------------------------------------

// Called from ClipperBase::AddPath().
TEdge* ClipperBase::ProcessBound(TEdge* E, bool NextIsForward)
{
  TEdge *Result = E;
  TEdge *Horz = 0;

  if (E->OutIdx == Skip)
  {
    //if edges still remain in the current bound beyond the skip edge then
    //create another LocMin and call ProcessBound once more
    if (NextIsForward)
    {
      while (E->Top.y() == E->Next->Bot.y()) E = E->Next;
      //don't include top horizontals when parsing a bound a second time,
      //they will be contained in the opposite bound ...
      while (E != Result && IsHorizontal(*E)) E = E->Prev;
    }
    else
    {
      while (E->Top.y() == E->Prev->Bot.y()) E = E->Prev;
      while (E != Result && IsHorizontal(*E)) E = E->Next;
    }

    if (E == Result)
    {
      if (NextIsForward) Result = E->Next;
      else Result = E->Prev;
    }
    else
    {
      //there are more edges in the bound beyond result starting with E
      if (NextIsForward)
        E = Result->Next;
      else
        E = Result->Prev;
      LocalMinimum locMin;
      locMin.Y = E->Bot.y();
      locMin.LeftBound = 0;
      locMin.RightBound = E;
      E->WindDelta = 0;
      Result = ProcessBound(E, NextIsForward);
      m_MinimaList.emplace_back(locMin);
    }
    return Result;
  }

  TEdge *EStart;

  if (IsHorizontal(*E))
  {
    //We need to be careful with open paths because this may not be a
    //true local minima (ie E may be following a skip edge).
    //Also, consecutive horz. edges may start heading left before going right.
    if (NextIsForward) 
      EStart = E->Prev;
    else 
      EStart = E->Next;
    if (IsHorizontal(*EStart)) //ie an adjoining horizontal skip edge
      {
        if (EStart->Bot.x() != E->Bot.x() && EStart->Top.x() != E->Bot.x())
          ReverseHorizontal(*E);
      }
      else if (EStart->Bot.x() != E->Bot.x())
        ReverseHorizontal(*E);
  }
  
  EStart = E;
  if (NextIsForward)
  {
    while (Result->Top.y() == Result->Next->Bot.y() && Result->Next->OutIdx != Skip)
      Result = Result->Next;
    if (IsHorizontal(*Result) && Result->Next->OutIdx != Skip)
    {
      //nb: at the top of a bound, horizontals are added to the bound
      //only when the preceding edge attaches to the horizontal's left vertex
      //unless a Skip edge is encountered when that becomes the top divide
      Horz = Result;
      while (IsHorizontal(*Horz->Prev)) Horz = Horz->Prev;
      if (Horz->Prev->Top.x() > Result->Next->Top.x()) Result = Horz->Prev;
    }
    while (E != Result) 
    {
      E->NextInLML = E->Next;
      if (IsHorizontal(*E) && E != EStart &&
        E->Bot.x() != E->Prev->Top.x()) ReverseHorizontal(*E);
      E = E->Next;
    }
    if (IsHorizontal(*E) && E != EStart && E->Bot.x() != E->Prev->Top.x()) 
      ReverseHorizontal(*E);
    Result = Result->Next; //move to the edge just beyond current bound
  } else
  {
    while (Result->Top.y() == Result->Prev->Bot.y() && Result->Prev->OutIdx != Skip) 
      Result = Result->Prev;
    if (IsHorizontal(*Result) && Result->Prev->OutIdx != Skip)
    {
      Horz = Result;
      while (IsHorizontal(*Horz->Next)) Horz = Horz->Next;
      if (Horz->Next->Top.x() == Result->Prev->Top.x() ||
          Horz->Next->Top.x() > Result->Prev->Top.x()) Result = Horz->Next;
    }

    while (E != Result)
    {
      E->NextInLML = E->Prev;
      if (IsHorizontal(*E) && E != EStart && E->Bot.x() != E->Next->Top.x()) 
        ReverseHorizontal(*E);
      E = E->Prev;
    }
    if (IsHorizontal(*E) && E != EStart && E->Bot.x() != E->Next->Top.x()) 
      ReverseHorizontal(*E);
    Result = Result->Prev; //move to the edge just beyond current bound
  }

  return Result;
}
//------------------------------------------------------------------------------

bool ClipperBase::AddPath(const Path &pg, PolyType PolyTyp, bool Closed)
{
  // Remove duplicate end point from a closed input path.
  // Remove duplicate points from the end of the input path.
  int highI = (int)pg.size() -1;
  if (Closed) 
    while (highI > 0 && (pg[highI] == pg[0])) 
      --highI;
  while (highI > 0 && (pg[highI] == pg[highI -1])) 
    --highI;
  if ((Closed && highI < 2) || (!Closed && highI < 1))
    return false;

  // Allocate a new edge array.
  Edges edges(highI + 1);
  // Fill in the edge array.
  bool result = AddPathInternal(pg, highI, PolyTyp, Closed, edges.data());
  if (result)
    // Success, remember the edge array.
    m_edges.emplace_back(std::move(edges));
  return result;
}

bool ClipperBase::AddPathInternal(const Path &pg, int highI, PolyType PolyTyp, bool Closed, TEdge* edges)
{
#ifdef use_lines
  if (!Closed && PolyTyp == ptClip)
    throw clipperException("AddPath: Open paths must be subject.");
#else
  if (!Closed)
    throw clipperException("AddPath: Open paths have been disabled.");
#endif

  assert(highI >= 0 && highI < pg.size());

  //1. Basic (first) edge initialization ...
  try
  {
    edges[1].Curr = pg[1];
#ifdef CLIPPERLIB_INT32
    RangeTest(pg[0]);
    RangeTest(pg[highI]);
#else
    RangeTest(pg[0], m_UseFullRange);
    RangeTest(pg[highI], m_UseFullRange);
#endif // CLIPPERLIB_INT32
    InitEdge(&edges[0], &edges[1], &edges[highI], pg[0]);
    InitEdge(&edges[highI], &edges[0], &edges[highI-1], pg[highI]);
    for (int i = highI - 1; i >= 1; --i)
    {
#ifdef CLIPPERLIB_INT32
      RangeTest(pg[i]);
#else
      RangeTest(pg[i], m_UseFullRange);
#endif // CLIPPERLIB_INT32
      InitEdge(&edges[i], &edges[i+1], &edges[i-1], pg[i]);
    }
  }
  catch(...)
  {
    throw; //range test fails
  }
  TEdge *eStart = &edges[0];

  //2. Remove duplicate vertices, and (when closed) collinear edges ...
  TEdge *E = eStart, *eLoopStop = eStart;
  for (;;)
  {
    //nb: allows matching start and end points when not Closed ...
    if (E->Curr == E->Next->Curr && (Closed || E->Next != eStart))
    {
      if (E == E->Next) break;
      if (E == eStart) eStart = E->Next;
      E = RemoveEdge(E);
      eLoopStop = E;
      continue;
    }
    if (E->Prev == E->Next) 
      break; //only two vertices
    else if (Closed &&
      SlopesEqual(E->Prev->Curr, E->Curr, E->Next->Curr, m_UseFullRange) && 
      (!m_PreserveCollinear ||
      !Pt2IsBetweenPt1AndPt3(E->Prev->Curr, E->Curr, E->Next->Curr)))
    {
      //Collinear edges are allowed for open paths but in closed paths
      //the default is to merge adjacent collinear edges into a single edge.
      //However, if the PreserveCollinear property is enabled, only overlapping
      //collinear edges (ie spikes) will be removed from closed paths.
      if (E == eStart) eStart = E->Next;
      E = RemoveEdge(E);
      E = E->Prev;
      eLoopStop = E;
      continue;
    }
    E = E->Next;
    if ((E == eLoopStop) || (!Closed && E->Next == eStart)) break;
  }

  if ((!Closed && (E == E->Next)) || (Closed && (E->Prev == E->Next)))
  {
    return false;
  }

  if (!Closed)
  { 
    m_HasOpenPaths = true;
    eStart->Prev->OutIdx = Skip;
  }

  //3. Do second stage of edge initialization ...
  // IsFlat means all vertices have the same Y coordinate.
  bool IsFlat = true;
  E = eStart;
  do
  {
    InitEdge2(*E, PolyTyp);
    E = E->Next;
    if (IsFlat && E->Curr.y() != eStart->Curr.y()) IsFlat = false;
  }
  while (E != eStart);

  //4. Finally, add edge bounds to LocalMinima list ...

  //Totally flat paths must be handled differently when adding them
  //to LocalMinima list to avoid endless loops etc ...
  if (IsFlat) 
  {
    if (Closed) 
    {
      return false;
    }
    E->Prev->OutIdx = Skip;
    LocalMinimum locMin;
    locMin.Y = E->Bot.y();
    locMin.LeftBound = 0;
    locMin.RightBound = E;
    locMin.RightBound->Side = esRight;
    locMin.RightBound->WindDelta = 0;
    for (;;)
    {
      if (E->Bot.x() != E->Prev->Top.x()) ReverseHorizontal(*E);
      if (E->Next->OutIdx == Skip) break;
      E->NextInLML = E->Next;
      E = E->Next;
    }
    m_MinimaList.emplace_back(locMin);
	  return true;
  }

  bool leftBoundIsForward;
  TEdge* EMin = 0;

  //workaround to avoid an endless loop in the while loop below when
  //open paths have matching start and end points ...
  if (E->Prev->Bot == E->Prev->Top) E = E->Next;

  // Find local minima and store them into a Local Minima List.
  // Multiple Local Minima could be created for a single path.
  for (;;)
  {
    E = FindNextLocMin(E);
    if (E == EMin) break;
    else if (!EMin) EMin = E;

    //E and E.Prev now share a local minima (left aligned if horizontal).
    //Compare their slopes to find which starts which bound ...
    LocalMinimum locMin;
    locMin.Y = E->Bot.y();
    if (E->Dx < E->Prev->Dx) 
    {
      locMin.LeftBound = E->Prev;
      locMin.RightBound = E;
      leftBoundIsForward = false; //Q.nextInLML = Q.prev
    } else
    {
      locMin.LeftBound = E;
      locMin.RightBound = E->Prev;
      leftBoundIsForward = true; //Q.nextInLML = Q.next
    }

    if (!Closed) locMin.LeftBound->WindDelta = 0;
    else if (locMin.LeftBound->Next == locMin.RightBound)
      locMin.LeftBound->WindDelta = -1;
    else locMin.LeftBound->WindDelta = 1;
    locMin.RightBound->WindDelta = -locMin.LeftBound->WindDelta;

    E = ProcessBound(locMin.LeftBound, leftBoundIsForward);
    if (E->OutIdx == Skip) E = ProcessBound(E, leftBoundIsForward);

    TEdge* E2 = ProcessBound(locMin.RightBound, !leftBoundIsForward);
    if (E2->OutIdx == Skip) E2 = ProcessBound(E2, !leftBoundIsForward);

    if (locMin.LeftBound->OutIdx == Skip)
      locMin.LeftBound = 0;
    else if (locMin.RightBound->OutIdx == Skip)
      locMin.RightBound = 0;
    m_MinimaList.emplace_back(locMin);
    if (!leftBoundIsForward) E = E2;
  }
  return true;
}
//------------------------------------------------------------------------------

void ClipperBase::Clear()
{
  m_MinimaList.clear();
  m_edges.clear();
#ifndef CLIPPERLIB_INT32
  m_UseFullRange = false;
#endif // CLIPPERLIB_INT32
  m_HasOpenPaths = false;
}
//------------------------------------------------------------------------------

// Initialize the Local Minima List:
// Sort the LML entries, initialize the left / right bound edges of each Local Minima.
void ClipperBase::Reset()
{
  if (m_MinimaList.empty()) return; //ie nothing to process
  std::sort(m_MinimaList.begin(), m_MinimaList.end(), [](const LocalMinimum& lm1, const LocalMinimum& lm2){ return lm1.Y < lm2.Y; });

  //reset all edges ...
  for (LocalMinimum &lm : m_MinimaList) {
    TEdge* e = lm.LeftBound;
    if (e)
    {
      e->Curr = e->Bot;
      e->Side = esLeft;
      e->OutIdx = Unassigned;
    }

    e = lm.RightBound;
    if (e)
    {
      e->Curr = e->Bot;
      e->Side = esRight;
      e->OutIdx = Unassigned;
    }
  }
}
//------------------------------------------------------------------------------

// Get bounds of the edges referenced by the Local Minima List.
// Returns (0,0,0,0) for an empty rectangle.
IntRect ClipperBase::GetBounds()
{
  IntRect result;
  auto lm = m_MinimaList.begin();
  if (lm == m_MinimaList.end())
  {
    result.left = result.top = result.right = result.bottom = 0;
    return result;
  }
  result.left = lm->LeftBound->Bot.x();
  result.top = lm->LeftBound->Bot.y();
  result.right = lm->LeftBound->Bot.x();
  result.bottom = lm->LeftBound->Bot.y();
  while (lm != m_MinimaList.end())
  {
    result.bottom = std::max(result.bottom, lm->LeftBound->Bot.y());
    TEdge* e = lm->LeftBound;
    for (;;) {
      TEdge* bottomE = e;
      while (e->NextInLML)
      {
        if (e->Bot.x() < result.left) result.left = e->Bot.x();
        if (e->Bot.x() > result.right) result.right = e->Bot.x();
        e = e->NextInLML;
      }
      result.left = std::min(result.left, e->Bot.x());
      result.right = std::max(result.right, e->Bot.x());
      result.left = std::min(result.left, e->Top.x());
      result.right = std::max(result.right, e->Top.x());
      result.top = std::min(result.top, e->Top.y());
      if (bottomE == lm->LeftBound) e = lm->RightBound;
      else break;
    }
    ++lm;
  }
  return result;
}

//------------------------------------------------------------------------------
// TClipper methods ...
//------------------------------------------------------------------------------

Clipper::Clipper(int initOptions) : 
  ClipperBase(),
  m_OutPtsFree(nullptr),
  m_OutPtsChunkLast(m_OutPtsChunkSize),
  m_ActiveEdges(nullptr),
  m_SortedEdges(nullptr)
{
  m_ReverseOutput = ((initOptions & ioReverseSolution) != 0);
  m_StrictSimple = ((initOptions & ioStrictlySimple) != 0);
  m_PreserveCollinear = ((initOptions & ioPreserveCollinear) != 0);
  m_HasOpenPaths = false;
#ifdef CLIPPERLIB_USE_XYZ  
  m_ZFill = 0;
#endif
}
//------------------------------------------------------------------------------

void Clipper::Reset()
{
  ClipperBase::Reset();
  m_Scanbeam = std::priority_queue<cInt, cInts>{};
  m_Maxima.clear();
  m_ActiveEdges = 0;
  m_SortedEdges = 0;
  for (auto lm = m_MinimaList.rbegin(); lm != m_MinimaList.rend(); ++lm)
    m_Scanbeam.push(lm->Y);
}

//------------------------------------------------------------------------------

bool Clipper::Execute(ClipType clipType, Paths &solution,
    PolyFillType subjFillType, PolyFillType clipFillType)
{
  if (m_HasOpenPaths)
    throw clipperException("Error: PolyTree struct is needed for open path clipping.");
  solution.clear();
  m_SubjFillType = subjFillType;
  m_ClipFillType = clipFillType;
  m_ClipType = clipType;
  m_UsingPolyTree = false;
  bool succeeded = ExecuteInternal();
  if (succeeded) BuildResult(solution);
  DisposeAllOutRecs();
  return succeeded;
}
//------------------------------------------------------------------------------

bool Clipper::Execute(ClipType clipType, PolyTree& polytree,
    PolyFillType subjFillType, PolyFillType clipFillType)
{
  m_SubjFillType = subjFillType;
  m_ClipFillType = clipFillType;
  m_ClipType = clipType;
  m_UsingPolyTree = true;
  bool succeeded = ExecuteInternal();
  if (succeeded) BuildResult2(polytree);
  DisposeAllOutRecs();
  return succeeded;
}
//------------------------------------------------------------------------------

bool Clipper::ExecuteInternal()
{
  bool succeeded = true;
  try {
    Reset();
    if (m_MinimaList.empty()) return true;
    cInt botY = m_Scanbeam.top();
    do { m_Scanbeam.pop(); } while (! m_Scanbeam.empty() && botY == m_Scanbeam.top());
    do {
      InsertLocalMinimaIntoAEL(botY);
      ProcessHorizontals();
	    m_GhostJoins.clear();
	    if (m_Scanbeam.empty()) break;
      cInt topY = m_Scanbeam.top();
      do { m_Scanbeam.pop(); } while (! m_Scanbeam.empty() && topY == m_Scanbeam.top());
      succeeded = ProcessIntersections(topY);
      if (!succeeded) break;
      ProcessEdgesAtTopOfScanbeam(topY);
      botY = topY;
    } while (!m_Scanbeam.empty() || !m_MinimaList.empty());
  }
  catch(...) 
  {
    succeeded = false;
  }

  if (succeeded)
  {

    //fix orientations ...
    //FIXME Vojtech: Does it not invalidate the loop hierarchy maintained as OutRec::FirstLeft pointers?
    //FIXME Vojtech: The area is calculated with floats, it may not be numerically stable!
    {
      for (OutRec &outRec : m_PolyOuts)
        if (outRec.Pts && !outRec.IsOpen && (outRec.IsHole ^ m_ReverseOutput) == (Area(outRec) > 0))
          ReversePolyPtLinks(outRec.Pts);
    }

    JoinCommonEdges();

    //unfortunately FixupOutPolygon() must be done after JoinCommonEdges()
    {
      for (OutRec &outRec : m_PolyOuts)
        if (outRec.Pts) {
          if (outRec.IsOpen)
            // Removes duplicate points.
            FixupOutPolyline(outRec);
          else
            // Removes duplicate points and simplifies consecutive parallel edges by removing the middle vertex.
            FixupOutPolygon(outRec);
        }
    }
    // For each polygon, search for exactly duplicate non-successive points.
    // If such a point is found, the loop is split into two pieces.
    // Search for the duplicate points is O(n^2)!
    // http://www.angusj.com/delphi/clipper/documentation/Docs/Units/ClipperLib/Classes/Clipper/Properties/StrictlySimple.htm
    if (m_StrictSimple) DoSimplePolygons();
  }

  m_Joins.clear();
  m_GhostJoins.clear();
  return succeeded;
}
//------------------------------------------------------------------------------

OutPt* Clipper::AllocateOutPt()
{
  OutPt *pt;
  if (m_OutPtsFree) {
    // Recycle some of the already released points.
    pt = m_OutPtsFree;
    m_OutPtsFree = pt->Next;
  } else if (m_OutPtsChunkLast < m_OutPtsChunkSize) {
    // Get a point from the last chunk.
    pt = &m_OutPts.back()[m_OutPtsChunkLast ++];
  } else {
    // The last chunk is full. Allocate a new one.
    m_OutPts.emplace_back();
    m_OutPtsChunkLast = 1;
    pt = &m_OutPts.back().front();
  }
  return pt;
}

void Clipper::DisposeAllOutRecs()
{
  m_OutPts.clear();
  m_OutPtsFree = nullptr;
  m_OutPtsChunkLast = m_OutPtsChunkSize;
  m_PolyOuts.clear();
}
//------------------------------------------------------------------------------

void Clipper::SetWindingCount(TEdge &edge) const
{
  TEdge *e = edge.PrevInAEL;
  //find the edge of the same polytype that immediately preceeds 'edge' in AEL
  while (e  && ((e->PolyTyp != edge.PolyTyp) || (e->WindDelta == 0))) e = e->PrevInAEL;
  if (!e)
  {
    if (edge.WindDelta == 0)
    {
      PolyFillType pft = (edge.PolyTyp == ptSubject ? m_SubjFillType : m_ClipFillType);
      edge.WindCnt = (pft == pftNegative ? -1 : 1);
    }
    else
      edge.WindCnt = edge.WindDelta;
    edge.WindCnt2 = 0;
    e = m_ActiveEdges; //ie get ready to calc WindCnt2
  }   
  else if (edge.WindDelta == 0 && m_ClipType != ctUnion)
  {
    edge.WindCnt = 1;
    edge.WindCnt2 = e->WindCnt2;
    e = e->NextInAEL; //ie get ready to calc WindCnt2
  }
  else if (IsEvenOddFillType(edge))
  {
    //EvenOdd filling ...
    if (edge.WindDelta == 0)
    {
      //are we inside a subj polygon ...
      bool Inside = true;
      TEdge *e2 = e->PrevInAEL;
      while (e2)
      {
        if (e2->PolyTyp == e->PolyTyp && e2->WindDelta != 0) 
          Inside = !Inside;
        e2 = e2->PrevInAEL;
      }
      edge.WindCnt = (Inside ? 0 : 1);
    }
    else
    {
      edge.WindCnt = edge.WindDelta;
    }
    edge.WindCnt2 = e->WindCnt2;
    e = e->NextInAEL; //ie get ready to calc WindCnt2
  } 
  else
  {
    //nonZero, Positive or Negative filling ...
    if (e->WindCnt * e->WindDelta < 0)
    {
      //prev edge is 'decreasing' WindCount (WC) toward zero
      //so we're outside the previous polygon ...
      if (std::abs(e->WindCnt) > 1)
      {
        //outside prev poly but still inside another.
        //when reversing direction of prev poly use the same WC 
        if (e->WindDelta * edge.WindDelta < 0) edge.WindCnt = e->WindCnt;
        //otherwise continue to 'decrease' WC ...
        else edge.WindCnt = e->WindCnt + edge.WindDelta;
      } 
      else
        //now outside all polys of same polytype so set own WC ...
        edge.WindCnt = (edge.WindDelta == 0 ? 1 : edge.WindDelta);
    } else
    {
      //prev edge is 'increasing' WindCount (WC) away from zero
      //so we're inside the previous polygon ...
      if (edge.WindDelta == 0) 
        edge.WindCnt = (e->WindCnt < 0 ? e->WindCnt - 1 : e->WindCnt + 1);
      //if wind direction is reversing prev then use same WC
      else if (e->WindDelta * edge.WindDelta < 0) edge.WindCnt = e->WindCnt;
      //otherwise add to WC ...
      else edge.WindCnt = e->WindCnt + edge.WindDelta;
    }
    edge.WindCnt2 = e->WindCnt2;
    e = e->NextInAEL; //ie get ready to calc WindCnt2
  }

  //update WindCnt2 ...
  if (IsEvenOddAltFillType(edge))
  {
    //EvenOdd filling ...
    while (e != &edge)
    {
      if (e->WindDelta != 0)
        edge.WindCnt2 = (edge.WindCnt2 == 0 ? 1 : 0);
      e = e->NextInAEL;
    }
  } else
  {
    //nonZero, Positive or Negative filling ...
    while ( e != &edge )
    {
      edge.WindCnt2 += e->WindDelta;
      e = e->NextInAEL;
    }
  }
}
//------------------------------------------------------------------------------

bool Clipper::IsContributing(const TEdge& edge) const
{
  PolyFillType pft, pft2;
  if (edge.PolyTyp == ptSubject)
  {
    pft = m_SubjFillType;
    pft2 = m_ClipFillType;
  } else
  {
    pft = m_ClipFillType;
    pft2 = m_SubjFillType;
  }

  switch(pft)
  {
    case pftEvenOdd: 
      //return false if a subj line has been flagged as inside a subj polygon
      if (edge.WindDelta == 0 && edge.WindCnt != 1) return false;
      break;
    case pftNonZero:
      if (std::abs(edge.WindCnt) != 1) return false;
      break;
    case pftPositive: 
      if (edge.WindCnt != 1) return false;
      break;
    default: //pftNegative
      if (edge.WindCnt != -1) return false;
  }

  switch(m_ClipType)
  {
    case ctIntersection:
      switch(pft2)
      {
        case pftEvenOdd: 
        case pftNonZero: 
          return (edge.WindCnt2 != 0);
        case pftPositive: 
          return (edge.WindCnt2 > 0);
        default: 
          return (edge.WindCnt2 < 0);
      }
      break;
    case ctUnion:
      switch(pft2)
      {
        case pftEvenOdd: 
        case pftNonZero: 
          return (edge.WindCnt2 == 0);
        case pftPositive: 
          return (edge.WindCnt2 <= 0);
        default: 
          return (edge.WindCnt2 >= 0);
      }
      break;
    case ctDifference:
      if (edge.PolyTyp == ptSubject)
        switch(pft2)
        {
          case pftEvenOdd: 
          case pftNonZero: 
            return (edge.WindCnt2 == 0);
          case pftPositive: 
            return (edge.WindCnt2 <= 0);
          default: 
            return (edge.WindCnt2 >= 0);
        }
      else
        switch(pft2)
        {
          case pftEvenOdd: 
          case pftNonZero: 
            return (edge.WindCnt2 != 0);
          case pftPositive: 
            return (edge.WindCnt2 > 0);
          default: 
            return (edge.WindCnt2 < 0);
        }
      break;
    case ctXor:
      if (edge.WindDelta == 0) //XOr always contributing unless open
        switch(pft2)
        {
          case pftEvenOdd: 
          case pftNonZero: 
            return (edge.WindCnt2 == 0);
          case pftPositive: 
            return (edge.WindCnt2 <= 0);
          default: 
            return (edge.WindCnt2 >= 0);
        }
      else 
        return true;
      break;
    default:
      return true;
  }
}
//------------------------------------------------------------------------------

// Called from Clipper::InsertLocalMinimaIntoAEL() and Clipper::IntersectEdges().
OutPt* Clipper::AddLocalMinPoly(TEdge *e1, TEdge *e2, const IntPoint &Pt)
{
  OutPt* result;
  TEdge *e, *prevE;
  if (IsHorizontal(*e2) || ( e1->Dx > e2->Dx ))
  {
    result = AddOutPt(e1, Pt);
    e2->OutIdx = e1->OutIdx;
    e1->Side = esLeft;
    e2->Side = esRight;
    e = e1;
    if (e->PrevInAEL == e2)
      prevE = e2->PrevInAEL; 
    else
      prevE = e->PrevInAEL;
  } else
  {
    result = AddOutPt(e2, Pt);
    e1->OutIdx = e2->OutIdx;
    e1->Side = esRight;
    e2->Side = esLeft;
    e = e2;
    if (e->PrevInAEL == e1)
        prevE = e1->PrevInAEL;
    else
        prevE = e->PrevInAEL;
  }

  if (prevE && prevE->OutIdx >= 0 && prevE->Top.y() < Pt.y() && e->Top.y() < Pt.y())
  {
    cInt xPrev = TopX(*prevE, Pt.y());
    cInt xE = TopX(*e, Pt.y());
    if (xPrev == xE && (e->WindDelta != 0) && (prevE->WindDelta != 0) &&
      SlopesEqual(IntPoint2d(xPrev, Pt.y()), prevE->Top, IntPoint2d(xE, Pt.y()), e->Top, m_UseFullRange))
    {
      OutPt* outPt = AddOutPt(prevE, Pt);
      m_Joins.emplace_back(Join(result, outPt, e->Top));
    }
  }
  return result;
}
//------------------------------------------------------------------------------

void Clipper::AddLocalMaxPoly(TEdge *e1, TEdge *e2, const IntPoint &Pt)
{
  AddOutPt( e1, Pt );
  if (e2->WindDelta == 0) AddOutPt(e2, Pt);
  if( e1->OutIdx == e2->OutIdx )
  {
    e1->OutIdx = Unassigned;
    e2->OutIdx = Unassigned;
  }
  else if (e1->OutIdx < e2->OutIdx) 
    AppendPolygon(e1, e2); 
  else 
    AppendPolygon(e2, e1);
}
//------------------------------------------------------------------------------

void Clipper::AddEdgeToSEL(TEdge *edge)
{
  //SEL pointers in PEdge are reused to build a list of horizontal edges.
  //However, we don't need to worry about order with horizontal edge processing.
  if( !m_SortedEdges )
  {
    m_SortedEdges = edge;
    edge->PrevInSEL = 0;
    edge->NextInSEL = 0;
  }
  else
  {
    edge->NextInSEL = m_SortedEdges;
    edge->PrevInSEL = 0;
    m_SortedEdges->PrevInSEL = edge;
    m_SortedEdges = edge;
  }
}
//------------------------------------------------------------------------------

void Clipper::CopyAELToSEL()
{
  TEdge* e = m_ActiveEdges;
  m_SortedEdges = e;
  while ( e )
  {
    e->PrevInSEL = e->PrevInAEL;
    e->NextInSEL = e->NextInAEL;
    e = e->NextInAEL;
  }
}

//------------------------------------------------------------------------------

// Called from Clipper::ExecuteInternal()
void Clipper::InsertLocalMinimaIntoAEL(const cInt botY)
{
  while (!m_MinimaList.empty() && m_MinimaList.back().Y == botY)
  {
    TEdge* lb = m_MinimaList.back().LeftBound;
    TEdge* rb = m_MinimaList.back().RightBound;
    m_MinimaList.pop_back();

    OutPt *Op1 = 0;
    if (!lb)
    {
      //nb: don't insert LB into either AEL or SEL
      InsertEdgeIntoAEL(rb, 0);
      SetWindingCount(*rb);
      if (IsContributing(*rb))
        Op1 = AddOutPt(rb, rb->Bot); 
    } 
    else if (!rb)
    {
      InsertEdgeIntoAEL(lb, 0);
      SetWindingCount(*lb);
      if (IsContributing(*lb))
        Op1 = AddOutPt(lb, lb->Bot);
      m_Scanbeam.push(lb->Top.y());
    }
    else
    {
      InsertEdgeIntoAEL(lb, 0);
      InsertEdgeIntoAEL(rb, lb);
      SetWindingCount( *lb );
      rb->WindCnt = lb->WindCnt;
      rb->WindCnt2 = lb->WindCnt2;
      if (IsContributing(*lb))
        Op1 = AddLocalMinPoly(lb, rb, lb->Bot);      
      m_Scanbeam.push(lb->Top.y());
    }

     if (rb)
     {
       if (IsHorizontal(*rb))
       {
         AddEdgeToSEL(rb);
         if (rb->NextInLML)
           m_Scanbeam.push(rb->NextInLML->Top.y());
       }
       else m_Scanbeam.push(rb->Top.y());
     }

    if (!lb || !rb) continue;

    //if any output polygons share an edge, they'll need joining later ...
    if (Op1 && IsHorizontal(*rb) && 
      m_GhostJoins.size() > 0 && (rb->WindDelta != 0))
    {
      for (Join &jr : m_GhostJoins)
        //if the horizontal Rb and a 'ghost' horizontal overlap, then convert
        //the 'ghost' join to a real join ready for later ...
        if (HorzSegmentsOverlap(jr.OutPt1->Pt.x(), jr.OffPt.x(), rb->Bot.x(), rb->Top.x()))
          m_Joins.emplace_back(Join(jr.OutPt1, Op1, jr.OffPt));
    }

    if (lb->OutIdx >= 0 && lb->PrevInAEL && 
      lb->PrevInAEL->Curr.x() == lb->Bot.x() &&
      lb->PrevInAEL->OutIdx >= 0 &&
      SlopesEqual(lb->PrevInAEL->Bot, lb->PrevInAEL->Top, lb->Curr, lb->Top, m_UseFullRange) &&
      (lb->WindDelta != 0) && (lb->PrevInAEL->WindDelta != 0))
    {
        OutPt *Op2 = AddOutPt(lb->PrevInAEL, lb->Bot);
        m_Joins.emplace_back(Join(Op1, Op2, lb->Top));
    }

    if(lb->NextInAEL != rb)
    {

      if (rb->OutIdx >= 0 && rb->PrevInAEL->OutIdx >= 0 &&
        SlopesEqual(rb->PrevInAEL->Curr, rb->PrevInAEL->Top, rb->Curr, rb->Top, m_UseFullRange) &&
        (rb->WindDelta != 0) && (rb->PrevInAEL->WindDelta != 0))
      {
          OutPt *Op2 = AddOutPt(rb->PrevInAEL, rb->Bot);
          m_Joins.emplace_back(Join(Op1, Op2, rb->Top));
      }

      TEdge* e = lb->NextInAEL;
      if (e)
      {
        while( e != rb )
        {
          //nb: For calculating winding counts etc, IntersectEdges() assumes
          //that param1 will be to the Right of param2 ABOVE the intersection ...
          IntersectEdges(rb , e , lb->Curr); //order important here
          e = e->NextInAEL;
        }
      }
    }
    
  }
}
//------------------------------------------------------------------------------

void Clipper::DeleteFromAEL(TEdge *e)
{
  TEdge* AelPrev = e->PrevInAEL;
  TEdge* AelNext = e->NextInAEL;
  if(  !AelPrev &&  !AelNext && (e != m_ActiveEdges) ) return; //already deleted
  if( AelPrev ) AelPrev->NextInAEL = AelNext;
  else m_ActiveEdges = AelNext;
  if( AelNext ) AelNext->PrevInAEL = AelPrev;
  e->NextInAEL = 0;
  e->PrevInAEL = 0;
}
//------------------------------------------------------------------------------

void Clipper::DeleteFromSEL(TEdge *e)
{
  TEdge* SelPrev = e->PrevInSEL;
  TEdge* SelNext = e->NextInSEL;
  if( !SelPrev &&  !SelNext && (e != m_SortedEdges) ) return; //already deleted
  if( SelPrev ) SelPrev->NextInSEL = SelNext;
  else m_SortedEdges = SelNext;
  if( SelNext ) SelNext->PrevInSEL = SelPrev;
  e->NextInSEL = 0;
  e->PrevInSEL = 0;
}
//------------------------------------------------------------------------------

#ifdef CLIPPERLIB_USE_XYZ
void Clipper::SetZ(IntPoint& pt, TEdge& e1, TEdge& e2)
{
  if (pt.z() != 0 || !m_ZFill) return;
  else if (pt == e1.Bot) pt.z() = e1.Bot.z();
  else if (pt == e1.Top) pt.z() = e1.Top.z();
  else if (pt == e2.Bot) pt.z() = e2.Bot.z();
  else if (pt == e2.Top) pt.z() = e2.Top.z();
  else m_ZFill(e1.Bot, e1.Top, e2.Bot, e2.Top, pt);
}
//------------------------------------------------------------------------------
#endif

void Clipper::IntersectEdges(TEdge *e1, TEdge *e2, IntPoint &Pt)
{
  bool e1Contributing = ( e1->OutIdx >= 0 );
  bool e2Contributing = ( e2->OutIdx >= 0 );

#ifdef CLIPPERLIB_USE_XYZ
        SetZ(Pt, *e1, *e2);
#endif

#ifdef use_lines
  //if either edge is on an OPEN path ...
  if (e1->WindDelta == 0 || e2->WindDelta == 0)
  {
    //ignore subject-subject open path intersections UNLESS they
    //are both open paths, AND they are both 'contributing maximas' ...
	if (e1->WindDelta == 0 && e2->WindDelta == 0) return;

    //if intersecting a subj line with a subj poly ...
    else if (e1->PolyTyp == e2->PolyTyp && 
      e1->WindDelta != e2->WindDelta && m_ClipType == ctUnion)
    {
      if (e1->WindDelta == 0)
      {
        if (e2Contributing)
        {
          AddOutPt(e1, Pt);
          if (e1Contributing) e1->OutIdx = Unassigned;
        }
      }
      else
      {
        if (e1Contributing)
        {
          AddOutPt(e2, Pt);
          if (e2Contributing) e2->OutIdx = Unassigned;
        }
      }
    }
    else if (e1->PolyTyp != e2->PolyTyp)
    {
      //toggle subj open path OutIdx on/off when Abs(clip.WndCnt) == 1 ...
      if ((e1->WindDelta == 0) && std::abs(e2->WindCnt) == 1 && 
        (m_ClipType != ctUnion || e2->WindCnt2 == 0))
      {
        AddOutPt(e1, Pt);
        if (e1Contributing) e1->OutIdx = Unassigned;
      }
      else if ((e2->WindDelta == 0) && (std::abs(e1->WindCnt) == 1) && 
        (m_ClipType != ctUnion || e1->WindCnt2 == 0))
      {
        AddOutPt(e2, Pt);
        if (e2Contributing) e2->OutIdx = Unassigned;
      }
    }
    return;
  }
#endif

  //update winding counts...
  //assumes that e1 will be to the Right of e2 ABOVE the intersection
  if ( e1->PolyTyp == e2->PolyTyp )
  {
    if ( IsEvenOddFillType( *e1) )
    {
      int oldE1WindCnt = e1->WindCnt;
      e1->WindCnt = e2->WindCnt;
      e2->WindCnt = oldE1WindCnt;
    } else
    {
      if (e1->WindCnt + e2->WindDelta == 0 ) e1->WindCnt = -e1->WindCnt;
      else e1->WindCnt += e2->WindDelta;
      if ( e2->WindCnt - e1->WindDelta == 0 ) e2->WindCnt = -e2->WindCnt;
      else e2->WindCnt -= e1->WindDelta;
    }
  } else
  {
    if (!IsEvenOddFillType(*e2)) e1->WindCnt2 += e2->WindDelta;
    else e1->WindCnt2 = ( e1->WindCnt2 == 0 ) ? 1 : 0;
    if (!IsEvenOddFillType(*e1)) e2->WindCnt2 -= e1->WindDelta;
    else e2->WindCnt2 = ( e2->WindCnt2 == 0 ) ? 1 : 0;
  }

  PolyFillType e1FillType, e2FillType, e1FillType2, e2FillType2;
  if (e1->PolyTyp == ptSubject)
  {
    e1FillType = m_SubjFillType;
    e1FillType2 = m_ClipFillType;
  } else
  {
    e1FillType = m_ClipFillType;
    e1FillType2 = m_SubjFillType;
  }
  if (e2->PolyTyp == ptSubject)
  {
    e2FillType = m_SubjFillType;
    e2FillType2 = m_ClipFillType;
  } else
  {
    e2FillType = m_ClipFillType;
    e2FillType2 = m_SubjFillType;
  }

  cInt e1Wc, e2Wc;
  switch (e1FillType)
  {
    case pftPositive: e1Wc = e1->WindCnt; break;
    case pftNegative: e1Wc = -e1->WindCnt; break;
    default: e1Wc = std::abs(e1->WindCnt);
  }
  switch(e2FillType)
  {
    case pftPositive: e2Wc = e2->WindCnt; break;
    case pftNegative: e2Wc = -e2->WindCnt; break;
    default: e2Wc = std::abs(e2->WindCnt);
  }

  if ( e1Contributing && e2Contributing )
  {
    if ((e1Wc != 0 && e1Wc != 1) || (e2Wc != 0 && e2Wc != 1) ||
      (e1->PolyTyp != e2->PolyTyp && m_ClipType != ctXor) )
    {
      AddLocalMaxPoly(e1, e2, Pt); 
    }
    else
    {
      AddOutPt(e1, Pt);
      AddOutPt(e2, Pt);
      std::swap(e1->Side, e2->Side);
      std::swap(e1->OutIdx, e2->OutIdx);
    }
  }
  else if ( e1Contributing )
  {
    if (e2Wc == 0 || e2Wc == 1) 
    {
      AddOutPt(e1, Pt);
      std::swap(e1->Side, e2->Side);
      std::swap(e1->OutIdx, e2->OutIdx);
    }
  }
  else if ( e2Contributing )
  {
    if (e1Wc == 0 || e1Wc == 1) 
    {
      AddOutPt(e2, Pt);
      std::swap(e1->Side, e2->Side);
      std::swap(e1->OutIdx, e2->OutIdx);
    }
  } 
  else if ( (e1Wc == 0 || e1Wc == 1) && (e2Wc == 0 || e2Wc == 1))
  {
    //neither edge is currently contributing ...

    cInt e1Wc2, e2Wc2;
    switch (e1FillType2)
    {
      case pftPositive: e1Wc2 = e1->WindCnt2; break;
      case pftNegative : e1Wc2 = -e1->WindCnt2; break;
      default: e1Wc2 = std::abs(e1->WindCnt2);
    }
    switch (e2FillType2)
    {
      case pftPositive: e2Wc2 = e2->WindCnt2; break;
      case pftNegative: e2Wc2 = -e2->WindCnt2; break;
      default: e2Wc2 = std::abs(e2->WindCnt2);
    }

    if (e1->PolyTyp != e2->PolyTyp)
    {
      AddLocalMinPoly(e1, e2, Pt);
    }
    else if (e1Wc == 1 && e2Wc == 1)
      switch( m_ClipType ) {
        case ctIntersection:
          if (e1Wc2 > 0 && e2Wc2 > 0)
            AddLocalMinPoly(e1, e2, Pt);
          break;
        case ctUnion:
          if ( e1Wc2 <= 0 && e2Wc2 <= 0 )
            AddLocalMinPoly(e1, e2, Pt);
          break;
        case ctDifference:
          if (((e1->PolyTyp == ptClip) && (e1Wc2 > 0) && (e2Wc2 > 0)) ||
              ((e1->PolyTyp == ptSubject) && (e1Wc2 <= 0) && (e2Wc2 <= 0)))
                AddLocalMinPoly(e1, e2, Pt);
          break;
        case ctXor:
          AddLocalMinPoly(e1, e2, Pt);
      }
    else
      std::swap(e1->Side, e2->Side);
  }
}
//------------------------------------------------------------------------------

void Clipper::SetHoleState(TEdge *e, OutRec *outrec)
{
  TEdge *e2 = e->PrevInAEL;
  TEdge *eTmp = 0;
  while (e2)
  {
    if (e2->OutIdx >= 0 && e2->WindDelta != 0)
    {
      if (!eTmp) eTmp = e2;
      else if (eTmp->OutIdx == e2->OutIdx) eTmp = 0;
    }
    e2 = e2->PrevInAEL;
  }
  if (!eTmp)
  {
    outrec->FirstLeft = 0;
    outrec->IsHole = false;
  }
  else
  {
    outrec->FirstLeft = &m_PolyOuts[eTmp->OutIdx];
    outrec->IsHole = !outrec->FirstLeft->IsHole;
  }
}
//------------------------------------------------------------------------------

OutRec* GetLowermostRec(OutRec *outRec1, OutRec *outRec2)
{
  //work out which polygon fragment has the correct hole state ...
  if (!outRec1->BottomPt) 
    outRec1->BottomPt = GetBottomPt(outRec1->Pts);
  if (!outRec2->BottomPt) 
    outRec2->BottomPt = GetBottomPt(outRec2->Pts);
  OutPt *OutPt1 = outRec1->BottomPt;
  OutPt *OutPt2 = outRec2->BottomPt;
  if (OutPt1->Pt.y() > OutPt2->Pt.y()) return outRec1;
  else if (OutPt1->Pt.y() < OutPt2->Pt.y()) return outRec2;
  else if (OutPt1->Pt.x() < OutPt2->Pt.x()) return outRec1;
  else if (OutPt1->Pt.x() > OutPt2->Pt.x()) return outRec2;
  else if (OutPt1->Next == OutPt1) return outRec2;
  else if (OutPt2->Next == OutPt2) return outRec1;
  else if (FirstIsBottomPt(OutPt1, OutPt2)) return outRec1;
  else return outRec2;
}
//------------------------------------------------------------------------------

bool OutRec1RightOfOutRec2(OutRec* outRec1, OutRec* outRec2)
{
  do
  {
    outRec1 = outRec1->FirstLeft;
    if (outRec1 == outRec2) return true;
  } while (outRec1);
  return false;
}
//------------------------------------------------------------------------------

OutRec* Clipper::GetOutRec(int Idx)
{
  OutRec* outrec = &m_PolyOuts[Idx];
  while (outrec != &m_PolyOuts[outrec->Idx])
    outrec = &m_PolyOuts[outrec->Idx];
  return outrec;
}
//------------------------------------------------------------------------------

void Clipper::AppendPolygon(TEdge *e1, TEdge *e2)
{
  //get the start and ends of both output polygons ...
  OutRec *outRec1 = &m_PolyOuts[e1->OutIdx];
  OutRec *outRec2 = &m_PolyOuts[e2->OutIdx];

  OutRec *holeStateRec;
  if (OutRec1RightOfOutRec2(outRec1, outRec2))
    holeStateRec = outRec2;
  else if (OutRec1RightOfOutRec2(outRec2, outRec1))
    holeStateRec = outRec1;
  else 
    holeStateRec = GetLowermostRec(outRec1, outRec2);

  //get the start and ends of both output polygons and
  //join e2 poly onto e1 poly and delete pointers to e2 ...

  OutPt* p1_lft = outRec1->Pts;
  OutPt* p1_rt = p1_lft->Prev;
  OutPt* p2_lft = outRec2->Pts;
  OutPt* p2_rt = p2_lft->Prev;

  //join e2 poly onto e1 poly and delete pointers to e2 ...
  if(  e1->Side == esLeft )
  {
    if(  e2->Side == esLeft )
    {
      //z y x a b c
      ReversePolyPtLinks(p2_lft);
      p2_lft->Next = p1_lft;
      p1_lft->Prev = p2_lft;
      p1_rt->Next = p2_rt;
      p2_rt->Prev = p1_rt;
      outRec1->Pts = p2_rt;
    } else
    {
      //x y z a b c
      p2_rt->Next = p1_lft;
      p1_lft->Prev = p2_rt;
      p2_lft->Prev = p1_rt;
      p1_rt->Next = p2_lft;
      outRec1->Pts = p2_lft;
    }
  } else
  {
    if(  e2->Side == esRight )
    {
      //a b c z y x
      ReversePolyPtLinks(p2_lft);
      p1_rt->Next = p2_rt;
      p2_rt->Prev = p1_rt;
      p2_lft->Next = p1_lft;
      p1_lft->Prev = p2_lft;
    } else
    {
      //a b c x y z
      p1_rt->Next = p2_lft;
      p2_lft->Prev = p1_rt;
      p1_lft->Prev = p2_rt;
      p2_rt->Next = p1_lft;
    }
  }

  outRec1->BottomPt = 0;
  if (holeStateRec == outRec2)
  {
    if (outRec2->FirstLeft != outRec1)
      outRec1->FirstLeft = outRec2->FirstLeft;
    outRec1->IsHole = outRec2->IsHole;
  }
  outRec2->Pts = 0;
  outRec2->BottomPt = 0;
  outRec2->FirstLeft = outRec1;

  int OKIdx = e1->OutIdx;
  int ObsoleteIdx = e2->OutIdx;

  e1->OutIdx = Unassigned; //nb: safe because we only get here via AddLocalMaxPoly
  e2->OutIdx = Unassigned;

  TEdge* e = m_ActiveEdges;
  while( e )
  {
    if( e->OutIdx == ObsoleteIdx )
    {
      e->OutIdx = OKIdx;
      e->Side = e1->Side;
      break;
    }
    e = e->NextInAEL;
  }

  outRec2->Idx = outRec1->Idx;
}
//------------------------------------------------------------------------------

OutRec* Clipper::CreateOutRec()
{
  m_PolyOuts.emplace_back();
  OutRec &result = m_PolyOuts.back();
  result.IsHole = false;
  result.IsOpen = false;
  result.FirstLeft = 0;
  result.Pts = 0;
  result.BottomPt = 0;
  result.PolyNd = 0;
  result.Idx = (int)m_PolyOuts.size()-1;
  return &result;
}
//------------------------------------------------------------------------------

OutPt* Clipper::AddOutPt(TEdge *e, const IntPoint &pt)
{
  if(  e->OutIdx < 0 )
  {
    OutRec *outRec = CreateOutRec();
    outRec->IsOpen = (e->WindDelta == 0);
    OutPt* newOp = this->AllocateOutPt();
    outRec->Pts = newOp;
    newOp->Idx = outRec->Idx;
    newOp->Pt = pt;
    newOp->Next = newOp;
    newOp->Prev = newOp;
    if (!outRec->IsOpen)
      SetHoleState(e, outRec);
    e->OutIdx = outRec->Idx;
    return newOp;
  } else
  {
    OutRec *outRec = &m_PolyOuts[e->OutIdx];
    //OutRec.Pts is the 'Left-most' point & OutRec.Pts.Prev is the 'Right-most'
    OutPt* op = outRec->Pts;

	bool ToFront = (e->Side == esLeft);
	if (ToFront && (pt == op->Pt)) return op;
    else if (!ToFront && (pt == op->Prev->Pt)) return op->Prev;

    OutPt* newOp = this->AllocateOutPt();
    newOp->Idx = outRec->Idx;
    newOp->Pt = pt;
    newOp->Next = op;
    newOp->Prev = op->Prev;
    newOp->Prev->Next = newOp;
    op->Prev = newOp;
    if (ToFront) outRec->Pts = newOp;
    return newOp;
  }
}
//------------------------------------------------------------------------------

OutPt* Clipper::GetLastOutPt(TEdge *e)
{
	OutRec *outRec = &m_PolyOuts[e->OutIdx];
	if (e->Side == esLeft)
		return outRec->Pts;
	else
		return outRec->Pts->Prev;
}
//------------------------------------------------------------------------------

void Clipper::ProcessHorizontals()
{
  TEdge* horzEdge = m_SortedEdges;
  while(horzEdge)
  {
    DeleteFromSEL(horzEdge);
    ProcessHorizontal(horzEdge);
    horzEdge = m_SortedEdges;
  }
}
//------------------------------------------------------------------------------

inline bool IsMaxima(TEdge *e, const cInt Y)
{
  return e && e->Top.y() == Y && !e->NextInLML;
}
//------------------------------------------------------------------------------

inline bool IsIntermediate(TEdge *e, const cInt Y)
{
  return e->Top.y() == Y && e->NextInLML;
}
//------------------------------------------------------------------------------

inline TEdge *GetMaximaPair(TEdge *e)
{
  if ((e->Next->Top == e->Top) && !e->Next->NextInLML)
    return e->Next;
  else if ((e->Prev->Top == e->Top) && !e->Prev->NextInLML)
    return e->Prev;
  else return 0;
}
//------------------------------------------------------------------------------

inline TEdge *GetMaximaPairEx(TEdge *e)
{
    //as GetMaximaPair() but returns 0 if MaxPair isn't in AEL (unless it's horizontal)
    TEdge* result = GetMaximaPair(e);
    if (result && (result->OutIdx == Skip ||
      (result->NextInAEL == result->PrevInAEL && !IsHorizontal(*result)))) return 0;
    return result;
}
//------------------------------------------------------------------------------

void Clipper::SwapPositionsInAEL(TEdge *Edge1, TEdge *Edge2)
{
  //check that one or other edge hasn't already been removed from AEL ...
  if (Edge1->NextInAEL == Edge1->PrevInAEL || 
    Edge2->NextInAEL == Edge2->PrevInAEL) return;

  if(  Edge1->NextInAEL == Edge2 )
  {
    TEdge* Next = Edge2->NextInAEL;
    if( Next ) Next->PrevInAEL = Edge1;
    TEdge* Prev = Edge1->PrevInAEL;
    if( Prev ) Prev->NextInAEL = Edge2;
    Edge2->PrevInAEL = Prev;
    Edge2->NextInAEL = Edge1;
    Edge1->PrevInAEL = Edge2;
    Edge1->NextInAEL = Next;
  }
  else if(  Edge2->NextInAEL == Edge1 )
  {
    TEdge* Next = Edge1->NextInAEL;
    if( Next ) Next->PrevInAEL = Edge2;
    TEdge* Prev = Edge2->PrevInAEL;
    if( Prev ) Prev->NextInAEL = Edge1;
    Edge1->PrevInAEL = Prev;
    Edge1->NextInAEL = Edge2;
    Edge2->PrevInAEL = Edge1;
    Edge2->NextInAEL = Next;
  }
  else
  {
    TEdge* Next = Edge1->NextInAEL;
    TEdge* Prev = Edge1->PrevInAEL;
    Edge1->NextInAEL = Edge2->NextInAEL;
    if( Edge1->NextInAEL ) Edge1->NextInAEL->PrevInAEL = Edge1;
    Edge1->PrevInAEL = Edge2->PrevInAEL;
    if( Edge1->PrevInAEL ) Edge1->PrevInAEL->NextInAEL = Edge1;
    Edge2->NextInAEL = Next;
    if( Edge2->NextInAEL ) Edge2->NextInAEL->PrevInAEL = Edge2;
    Edge2->PrevInAEL = Prev;
    if( Edge2->PrevInAEL ) Edge2->PrevInAEL->NextInAEL = Edge2;
  }

  if( !Edge1->PrevInAEL ) m_ActiveEdges = Edge1;
  else if( !Edge2->PrevInAEL ) m_ActiveEdges = Edge2;
}
//------------------------------------------------------------------------------

void Clipper::SwapPositionsInSEL(TEdge *Edge1, TEdge *Edge2)
{
  if(  !( Edge1->NextInSEL ) &&  !( Edge1->PrevInSEL ) ) return;
  if(  !( Edge2->NextInSEL ) &&  !( Edge2->PrevInSEL ) ) return;

  if(  Edge1->NextInSEL == Edge2 )
  {
    TEdge* Next = Edge2->NextInSEL;
    if( Next ) Next->PrevInSEL = Edge1;
    TEdge* Prev = Edge1->PrevInSEL;
    if( Prev ) Prev->NextInSEL = Edge2;
    Edge2->PrevInSEL = Prev;
    Edge2->NextInSEL = Edge1;
    Edge1->PrevInSEL = Edge2;
    Edge1->NextInSEL = Next;
  }
  else if(  Edge2->NextInSEL == Edge1 )
  {
    TEdge* Next = Edge1->NextInSEL;
    if( Next ) Next->PrevInSEL = Edge2;
    TEdge* Prev = Edge2->PrevInSEL;
    if( Prev ) Prev->NextInSEL = Edge1;
    Edge1->PrevInSEL = Prev;
    Edge1->NextInSEL = Edge2;
    Edge2->PrevInSEL = Edge1;
    Edge2->NextInSEL = Next;
  }
  else
  {
    TEdge* Next = Edge1->NextInSEL;
    TEdge* Prev = Edge1->PrevInSEL;
    Edge1->NextInSEL = Edge2->NextInSEL;
    if( Edge1->NextInSEL ) Edge1->NextInSEL->PrevInSEL = Edge1;
    Edge1->PrevInSEL = Edge2->PrevInSEL;
    if( Edge1->PrevInSEL ) Edge1->PrevInSEL->NextInSEL = Edge1;
    Edge2->NextInSEL = Next;
    if( Edge2->NextInSEL ) Edge2->NextInSEL->PrevInSEL = Edge2;
    Edge2->PrevInSEL = Prev;
    if( Edge2->PrevInSEL ) Edge2->PrevInSEL->NextInSEL = Edge2;
  }

  if( !Edge1->PrevInSEL ) m_SortedEdges = Edge1;
  else if( !Edge2->PrevInSEL ) m_SortedEdges = Edge2;
}
//------------------------------------------------------------------------------

inline void GetHorzDirection(TEdge& HorzEdge, Direction& Dir, cInt& Left, cInt& Right)
{
  if (HorzEdge.Bot.x() < HorzEdge.Top.x())
  {
    Left = HorzEdge.Bot.x();
    Right = HorzEdge.Top.x();
    Dir = dLeftToRight;
  } else
  {
    Left = HorzEdge.Top.x();
    Right = HorzEdge.Bot.x();
    Dir = dRightToLeft;
  }
}
//------------------------------------------------------------------------

/*******************************************************************************
* Notes: Horizontal edges (HEs) at scanline intersections (ie at the Top or    *
* Bottom of a scanbeam) are processed as if layered. The order in which HEs    *
* are processed doesn't matter. HEs intersect with other HE Bot.x()s only [#]    *
* (or they could intersect with Top.x()s only, ie EITHER Bot.x()s OR Top.x()s),      *
* and with other non-horizontal edges [*]. Once these intersections are        *
* processed, intermediate HEs then 'promote' the Edge above (NextInLML) into   *
* the AEL. These 'promoted' edges may in turn intersect [%] with other HEs.    *
*******************************************************************************/

void Clipper::ProcessHorizontal(TEdge *horzEdge)
{
  Direction dir;
  cInt horzLeft, horzRight;
  bool IsOpen = (horzEdge->WindDelta == 0);

  GetHorzDirection(*horzEdge, dir, horzLeft, horzRight);

  TEdge* eLastHorz = horzEdge, *eMaxPair = 0;
  while (eLastHorz->NextInLML && IsHorizontal(*eLastHorz->NextInLML)) 
    eLastHorz = eLastHorz->NextInLML;
  if (!eLastHorz->NextInLML)
    eMaxPair = GetMaximaPair(eLastHorz);

  cInts::const_iterator maxIt;
  cInts::const_reverse_iterator maxRit;
  if (!m_Maxima.empty())
  {
      //get the first maxima in range (X) ...
      if (dir == dLeftToRight)
      {
          maxIt = m_Maxima.begin();
          while (maxIt != m_Maxima.end() && *maxIt <= horzEdge->Bot.x()) ++maxIt;
          if (maxIt != m_Maxima.end() && *maxIt >= eLastHorz->Top.x())
              maxIt = m_Maxima.end();
      }
      else
      {
          maxRit = m_Maxima.rbegin();
          while (maxRit != m_Maxima.rend() && *maxRit > horzEdge->Bot.x()) ++maxRit;
          if (maxRit != m_Maxima.rend() && *maxRit <= eLastHorz->Top.x())
              maxRit = m_Maxima.rend();
      }
  }

  OutPt* op1 = 0;

  for (;;) //loop through consec. horizontal edges
  {
		  
    bool IsLastHorz = (horzEdge == eLastHorz);
    TEdge* e = (dir == dLeftToRight) ? horzEdge->NextInAEL : horzEdge->PrevInAEL;
    while(e)
    {

        //this code block inserts extra coords into horizontal edges (in output
        //polygons) whereever maxima touch these horizontal edges. This helps
        //'simplifying' polygons (ie if the Simplify property is set).
        if (!m_Maxima.empty())
        {
            if (dir == dLeftToRight)
            {
                while (maxIt != m_Maxima.end() && *maxIt < e->Curr.x()) 
                {
                  if (horzEdge->OutIdx >= 0 && !IsOpen)
                    AddOutPt(horzEdge, IntPoint2d(*maxIt, horzEdge->Bot.y()));
                  ++maxIt;
                }
            }
            else
            {
                while (maxRit != m_Maxima.rend() && *maxRit > e->Curr.x())
                {
                  if (horzEdge->OutIdx >= 0 && !IsOpen)
                    AddOutPt(horzEdge, IntPoint2d(*maxRit, horzEdge->Bot.y()));
                  ++maxRit;
                }
            }
        };

        if ((dir == dLeftToRight && e->Curr.x() > horzRight) ||
			(dir == dRightToLeft && e->Curr.x() < horzLeft)) break;

		//Also break if we've got to the end of an intermediate horizontal edge ...
		//nb: Smaller Dx's are to the right of larger Dx's ABOVE the horizontal.
		if (e->Curr.x() == horzEdge->Top.x() && horzEdge->NextInLML && 
			e->Dx < horzEdge->NextInLML->Dx) break;

    if (horzEdge->OutIdx >= 0 && !IsOpen)  //note: may be done multiple times
		{
#ifdef CLIPPERLIB_USE_XYZ
            if (dir == dLeftToRight)
                SetZ(e->Curr, *horzEdge, *e);
            else
                SetZ(e->Curr, *e, *horzEdge);
#endif
            op1 = AddOutPt(horzEdge, e->Curr);
			TEdge* eNextHorz = m_SortedEdges;
			while (eNextHorz)
			{
				if (eNextHorz->OutIdx >= 0 &&
					HorzSegmentsOverlap(horzEdge->Bot.x(),
					horzEdge->Top.x(), eNextHorz->Bot.x(), eNextHorz->Top.x()))
				{
                    OutPt* op2 = GetLastOutPt(eNextHorz);
                    m_Joins.emplace_back(Join(op2, op1, eNextHorz->Top));
				}
				eNextHorz = eNextHorz->NextInSEL;
			}
      m_GhostJoins.emplace_back(Join(op1, 0, horzEdge->Bot));
		}
		
		//OK, so far we're still in range of the horizontal Edge  but make sure
        //we're at the last of consec. horizontals when matching with eMaxPair
        if(e == eMaxPair && IsLastHorz)
        {
          if (horzEdge->OutIdx >= 0)
            AddLocalMaxPoly(horzEdge, eMaxPair, horzEdge->Top);
          DeleteFromAEL(horzEdge);
          DeleteFromAEL(eMaxPair);
          return;
        }
        
		if(dir == dLeftToRight)
        {
          IntPoint Pt = IntPoint2d(e->Curr.x(), horzEdge->Curr.y());
          IntersectEdges(horzEdge, e, Pt);
        }
        else
        {
          IntPoint Pt = IntPoint2d(e->Curr.x(), horzEdge->Curr.y());
          IntersectEdges( e, horzEdge, Pt);
        }
        TEdge* eNext = (dir == dLeftToRight) ? e->NextInAEL : e->PrevInAEL;
        SwapPositionsInAEL( horzEdge, e );
        e = eNext;
    } //end while(e)

	//Break out of loop if HorzEdge.NextInLML is not also horizontal ...
	if (!horzEdge->NextInLML || !IsHorizontal(*horzEdge->NextInLML)) break;

	UpdateEdgeIntoAEL(horzEdge);
    if (horzEdge->OutIdx >= 0) AddOutPt(horzEdge, horzEdge->Bot);
    GetHorzDirection(*horzEdge, dir, horzLeft, horzRight);

  } //end for (;;)

  if (horzEdge->OutIdx >= 0 && !op1)
  {
      op1 = GetLastOutPt(horzEdge);
      TEdge* eNextHorz = m_SortedEdges;
      while (eNextHorz)
      {
          if (eNextHorz->OutIdx >= 0 &&
              HorzSegmentsOverlap(horzEdge->Bot.x(),
              horzEdge->Top.x(), eNextHorz->Bot.x(), eNextHorz->Top.x()))
          {
              OutPt* op2 = GetLastOutPt(eNextHorz);
              m_Joins.emplace_back(Join(op2, op1, eNextHorz->Top));
          }
          eNextHorz = eNextHorz->NextInSEL;
      }
      m_GhostJoins.emplace_back(Join(op1, 0, horzEdge->Top));
  }

  if (horzEdge->NextInLML)
  {
    if(horzEdge->OutIdx >= 0)
    {
      op1 = AddOutPt( horzEdge, horzEdge->Top);
      UpdateEdgeIntoAEL(horzEdge);
      if (horzEdge->WindDelta == 0) return;
      //nb: HorzEdge is no longer horizontal here
      TEdge* ePrev = horzEdge->PrevInAEL;
      TEdge* eNext = horzEdge->NextInAEL;
      if (ePrev && ePrev->Curr.x() == horzEdge->Bot.x() &&
        ePrev->Curr.y() == horzEdge->Bot.y() && ePrev->WindDelta != 0 &&
        (ePrev->OutIdx >= 0 && ePrev->Curr.y() > ePrev->Top.y() &&
        SlopesEqual(*horzEdge, *ePrev, m_UseFullRange)))
      {
        OutPt* op2 = AddOutPt(ePrev, horzEdge->Bot);
        m_Joins.emplace_back(Join(op1, op2, horzEdge->Top));
      }
      else if (eNext && eNext->Curr.x() == horzEdge->Bot.x() &&
        eNext->Curr.y() == horzEdge->Bot.y() && eNext->WindDelta != 0 &&
        eNext->OutIdx >= 0 && eNext->Curr.y() > eNext->Top.y() &&
        SlopesEqual(*horzEdge, *eNext, m_UseFullRange))
      {
        OutPt* op2 = AddOutPt(eNext, horzEdge->Bot);
        m_Joins.emplace_back(Join(op1, op2, horzEdge->Top));
      }
    }
    else
      UpdateEdgeIntoAEL(horzEdge); 
  }
  else
  {
    if (horzEdge->OutIdx >= 0) AddOutPt(horzEdge, horzEdge->Top);
    DeleteFromAEL(horzEdge);
  }
}
//------------------------------------------------------------------------------

void Clipper::UpdateEdgeIntoAEL(TEdge *&e)
{
  if( !e->NextInLML ) 
    throw clipperException("UpdateEdgeIntoAEL: invalid call");

  e->NextInLML->OutIdx = e->OutIdx;
  TEdge* AelPrev = e->PrevInAEL;
  TEdge* AelNext = e->NextInAEL;
  if (AelPrev) AelPrev->NextInAEL = e->NextInLML;
  else m_ActiveEdges = e->NextInLML;
  if (AelNext) AelNext->PrevInAEL = e->NextInLML;
  e->NextInLML->Side = e->Side;
  e->NextInLML->WindDelta = e->WindDelta;
  e->NextInLML->WindCnt = e->WindCnt;
  e->NextInLML->WindCnt2 = e->WindCnt2;
  e = e->NextInLML;
  e->Curr = e->Bot;
  e->PrevInAEL = AelPrev;
  e->NextInAEL = AelNext;
  if (!IsHorizontal(*e)) 
    m_Scanbeam.push(e->Top.y());
}
//------------------------------------------------------------------------------

bool Clipper::ProcessIntersections(const cInt topY)
{
  if( !m_ActiveEdges ) return true;
  try {
    BuildIntersectList(topY);
    size_t IlSize = m_IntersectList.size();
    if (IlSize == 0) return true;
    if (IlSize == 1 || FixupIntersectionOrder()) {
      for (IntersectNode &iNode : m_IntersectList) {
        IntersectEdges( iNode.Edge1, iNode.Edge2, iNode.Pt);
        SwapPositionsInAEL( iNode.Edge1 , iNode.Edge2 );
      }
      m_IntersectList.clear();
    }
    else return false;
  }
  catch(...) 
  {
    m_SortedEdges = 0;
    m_IntersectList.clear();
    throw clipperException("ProcessIntersections error");
  }
  m_SortedEdges = 0;
  return true;
}
//------------------------------------------------------------------------------

void Clipper::BuildIntersectList(const cInt topY)
{
  if ( !m_ActiveEdges ) return;

  //prepare for sorting ...
  TEdge* e = m_ActiveEdges;
  m_SortedEdges = e;
  while( e )
  {
    e->PrevInSEL = e->PrevInAEL;
    e->NextInSEL = e->NextInAEL;
    e->Curr.x() = TopX( *e, topY );
    e = e->NextInAEL;
  }

  //bubblesort ...
  bool isModified;
  do
  {
    isModified = false;
    e = m_SortedEdges;
    while( e->NextInSEL )
    {
      TEdge *eNext = e->NextInSEL;
      IntPoint Pt;
      if(e->Curr.x() > eNext->Curr.x())
      {
        IntersectPoint(*e, *eNext, Pt);

        if (Pt.y() < topY) Pt = IntPoint2d(TopX(*e, topY), topY);
        m_IntersectList.emplace_back(IntersectNode(e, eNext, Pt));
        SwapPositionsInSEL(e, eNext);
        isModified = true;
      }
      else
        e = eNext;
    }
    if( e->PrevInSEL ) e->PrevInSEL->NextInSEL = 0;
    else break;
  }
  while ( isModified );
  m_SortedEdges = 0; //important
}
//------------------------------------------------------------------------------


inline bool EdgesAdjacent(const IntersectNode &inode)
{
  return (inode.Edge1->NextInSEL == inode.Edge2) ||
    (inode.Edge1->PrevInSEL == inode.Edge2);
}
//------------------------------------------------------------------------------

bool Clipper::FixupIntersectionOrder()
{
  //pre-condition: intersections are sorted Bottom-most first.
  //Now it's crucial that intersections are made only between adjacent edges,
  //so to ensure this the order of intersections may need adjusting ...
  CopyAELToSEL();
  std::sort(m_IntersectList.begin(), m_IntersectList.end(), [](const IntersectNode &node1, const IntersectNode &node2) { return node2.Pt.y() < node1.Pt.y(); });

  size_t cnt = m_IntersectList.size();
  for (size_t i = 0; i < cnt; ++i) 
  {
    if (!EdgesAdjacent(m_IntersectList[i]))
    {
      size_t j = i + 1;
      while (j < cnt && !EdgesAdjacent(m_IntersectList[j])) j++;
      if (j == cnt)  return false;
      std::swap(m_IntersectList[i], m_IntersectList[j]);
    }
    SwapPositionsInSEL(m_IntersectList[i].Edge1, m_IntersectList[i].Edge2);
  }
  return true;
}
//------------------------------------------------------------------------------

void Clipper::DoMaxima(TEdge *e)
{
  TEdge* eMaxPair = GetMaximaPairEx(e);
  if (!eMaxPair)
  {
    if (e->OutIdx >= 0)
      AddOutPt(e, e->Top);
    DeleteFromAEL(e);
    return;
  }

  TEdge* eNext = e->NextInAEL;
  while(eNext && eNext != eMaxPair)
  {
    IntersectEdges(e, eNext, e->Top);
    SwapPositionsInAEL(e, eNext);
    eNext = e->NextInAEL;
  }

  if(e->OutIdx == Unassigned && eMaxPair->OutIdx == Unassigned)
  {
    DeleteFromAEL(e);
    DeleteFromAEL(eMaxPair);
  }
  else if( e->OutIdx >= 0 && eMaxPair->OutIdx >= 0 )
  {
    if (e->OutIdx >= 0) AddLocalMaxPoly(e, eMaxPair, e->Top);
    DeleteFromAEL(e);
    DeleteFromAEL(eMaxPair);
  }
#ifdef use_lines
  else if (e->WindDelta == 0)
  {
    if (e->OutIdx >= 0) 
    {
      AddOutPt(e, e->Top);
      e->OutIdx = Unassigned;
    }
    DeleteFromAEL(e);

    if (eMaxPair->OutIdx >= 0)
    {
      AddOutPt(eMaxPair, e->Top);
      eMaxPair->OutIdx = Unassigned;
    }
    DeleteFromAEL(eMaxPair);
  } 
#endif
  else throw clipperException("DoMaxima error");
}
//------------------------------------------------------------------------------

void Clipper::ProcessEdgesAtTopOfScanbeam(const cInt topY)
{
  TEdge* e = m_ActiveEdges;
  while( e )
  {
    //1. process maxima, treating them as if they're 'bent' horizontal edges,
    //   but exclude maxima with horizontal edges. nb: e can't be a horizontal.
    bool IsMaximaEdge = IsMaxima(e, topY);

    if(IsMaximaEdge)
    {
      TEdge* eMaxPair = GetMaximaPairEx(e);
      IsMaximaEdge = (!eMaxPair || !IsHorizontal(*eMaxPair));
    }

    if(IsMaximaEdge)
    {
      if (m_StrictSimple) m_Maxima.emplace_back(e->Top.x());
      TEdge* ePrev = e->PrevInAEL;
      DoMaxima(e);
      if( !ePrev ) e = m_ActiveEdges;
      else e = ePrev->NextInAEL;
    }
    else
    {
      //2. promote horizontal edges, otherwise update Curr.x() and Curr.y() ...
      if (IsIntermediate(e, topY) && IsHorizontal(*e->NextInLML))
      {
        UpdateEdgeIntoAEL(e);
        if (e->OutIdx >= 0)
          AddOutPt(e, e->Bot);
        AddEdgeToSEL(e);
      } 
      else
      {
        e->Curr.x() = TopX( *e, topY );
        e->Curr.y() = topY;
#ifdef CLIPPERLIB_USE_XYZ
        e->Curr.z() = topY == e->Top.y() ? e->Top.z() : (topY == e->Bot.y() ? e->Bot.z() : 0);
#endif
      }

      //When StrictlySimple and 'e' is being touched by another edge, then
      //make sure both edges have a vertex here ...
      if (m_StrictSimple)
      {  
        TEdge* ePrev = e->PrevInAEL;
        if ((e->OutIdx >= 0) && (e->WindDelta != 0) && ePrev && (ePrev->OutIdx >= 0) &&
          (ePrev->Curr.x() == e->Curr.x()) && (ePrev->WindDelta != 0))
        {
          IntPoint pt = e->Curr;
#ifdef CLIPPERLIB_USE_XYZ
          SetZ(pt, *ePrev, *e);
#endif
          OutPt* op = AddOutPt(ePrev, pt);
          OutPt* op2 = AddOutPt(e, pt);
          m_Joins.emplace_back(Join(op, op2, pt)); //StrictlySimple (type-3) join
        }
      }

      e = e->NextInAEL;
    }
  }

  //3. Process horizontals at the Top of the scanbeam ...
  std::sort(m_Maxima.begin(), m_Maxima.end());
  ProcessHorizontals();
  m_Maxima.clear();

  //4. Promote intermediate vertices ...
  e = m_ActiveEdges;
  while(e)
  {
    if(IsIntermediate(e, topY))
    {
      OutPt* op = 0;
      if( e->OutIdx >= 0 ) 
        op = AddOutPt(e, e->Top);
      UpdateEdgeIntoAEL(e);

      //if output polygons share an edge, they'll need joining later ...
      TEdge* ePrev = e->PrevInAEL;
      TEdge* eNext = e->NextInAEL;
      if (ePrev && ePrev->Curr.x() == e->Bot.x() &&
        ePrev->Curr.y() == e->Bot.y() && op &&
        ePrev->OutIdx >= 0 && ePrev->Curr.y() > ePrev->Top.y() &&
        SlopesEqual(e->Curr, e->Top, ePrev->Curr, ePrev->Top, m_UseFullRange) &&
        (e->WindDelta != 0) && (ePrev->WindDelta != 0))
      {
        OutPt* op2 = AddOutPt(ePrev, e->Bot);
        m_Joins.emplace_back(Join(op, op2, e->Top));
      }
      else if (eNext && eNext->Curr.x() == e->Bot.x() &&
        eNext->Curr.y() == e->Bot.y() && op &&
        eNext->OutIdx >= 0 && eNext->Curr.y() > eNext->Top.y() &&
        SlopesEqual(e->Curr, e->Top, eNext->Curr, eNext->Top, m_UseFullRange) &&
        (e->WindDelta != 0) && (eNext->WindDelta != 0))
      {
        OutPt* op2 = AddOutPt(eNext, e->Bot);
        m_Joins.emplace_back(Join(op, op2, e->Top));
      }
    }
    e = e->NextInAEL;
  }
}
//------------------------------------------------------------------------------

void Clipper::FixupOutPolyline(OutRec &outrec)
{
  OutPt *pp = outrec.Pts;
  OutPt *lastPP = pp->Prev;
  while (pp != lastPP)
  {
    pp = pp->Next;
    if (pp->Pt == pp->Prev->Pt)
    {
      if (pp == lastPP) lastPP = pp->Prev;
      OutPt *tmpPP = pp->Prev;
      tmpPP->Next = pp->Next;
      pp->Next->Prev = tmpPP;
      this->DisposeOutPt(pp);
      pp = tmpPP;
    }
  }

  if (pp == pp->Prev)
  {
    this->DisposeOutPts(pp);
    outrec.Pts = 0;
    return;
  }
}
//------------------------------------------------------------------------------

void Clipper::FixupOutPolygon(OutRec &outrec)
{
    //FixupOutPolygon() - removes duplicate points and simplifies consecutive
    //parallel edges by removing the middle vertex.
    OutPt *lastOK = nullptr;
    outrec.BottomPt = nullptr;
    OutPt *pp = outrec.Pts;
    bool preserveCol = m_PreserveCollinear || m_StrictSimple;

    for (;;)
    {
        if (pp->Prev == pp || pp->Prev == pp->Next)
        {
            // Empty loop or a stick. Release the polygon.
            this->DisposeOutPts(pp);
            outrec.Pts = nullptr;
            return;
        }

        //test for duplicate points and collinear edges ...
        if ((pp->Pt == pp->Next->Pt) || (pp->Pt == pp->Prev->Pt) ||
            (SlopesEqual(pp->Prev->Pt, pp->Pt, pp->Next->Pt, m_UseFullRange) &&
            (!preserveCol || !Pt2IsBetweenPt1AndPt3(pp->Prev->Pt, pp->Pt, pp->Next->Pt))))
        {
            lastOK = nullptr;
            OutPt *tmp = pp;
            pp->Prev->Next = pp->Next;
            pp->Next->Prev = pp->Prev;
            pp = pp->Prev;
            this->DisposeOutPt(tmp);
        }
        else if (pp == lastOK) break;
        else
        {
            if (!lastOK) lastOK = pp;
            pp = pp->Next;
        }
    }
    outrec.Pts = pp;
}
//------------------------------------------------------------------------------

// Count the number of points in a closed linked loop starting with Pts.
int PointCount(OutPt *Pts)
{
    if (!Pts) return 0;
    int result = 0;
    OutPt* p = Pts;
    do
    {
        result++;
        p = p->Next;
    }
    while (p != Pts);
    return result;
}
//------------------------------------------------------------------------------

void Clipper::BuildResult(Paths &polys)
{
  polys.reserve(m_PolyOuts.size());
  for (OutRec &outRec : m_PolyOuts)
  {
    assert(! outRec.IsOpen);
    if (!outRec.Pts) continue;
    Path pg;
    OutPt* p = outRec.Pts->Prev;
    int cnt = PointCount(p);
    if (cnt < 2) continue;
    pg.reserve(cnt);
    for (int i = 0; i < cnt; ++i)
    {
      pg.emplace_back(p->Pt);
      p = p->Prev;
    }
    polys.emplace_back(std::move(pg));
  }
}
//------------------------------------------------------------------------------

void Clipper::BuildResult2(PolyTree& polytree)
{
    polytree.Clear();
    polytree.AllNodes.reserve(m_PolyOuts.size());
    //add each output polygon/contour to polytree ...
    for (OutRec &outRec : m_PolyOuts)
    {
        int cnt = PointCount(outRec.Pts);
        if ((outRec.IsOpen && cnt < 2) || (!outRec.IsOpen && cnt < 3))
          // Ignore an invalid output loop or a polyline.
          continue;

        //skip OutRecs that (a) contain outermost polygons or
        //(b) already have the correct owner/child linkage ...
        if (outRec.FirstLeft &&
            (outRec.IsHole == outRec.FirstLeft->IsHole || ! outRec.FirstLeft->Pts)) {
          OutRec* orfl = outRec.FirstLeft;
          while (orfl && ((orfl->IsHole == outRec.IsHole) || !orfl->Pts))
              orfl = orfl->FirstLeft;
          outRec.FirstLeft = orfl;
        }

        //nb: polytree takes ownership of all the PolyNodes
        polytree.AllNodes.emplace_back(PolyNode());
        PolyNode* pn = &polytree.AllNodes.back();
        outRec.PolyNd = pn;
        pn->Parent = 0;
        pn->Index = 0;
        pn->Contour.reserve(cnt);
        OutPt *op = outRec.Pts->Prev;
        for (int j = 0; j < cnt; j++)
        {
            pn->Contour.emplace_back(op->Pt);
            op = op->Prev;
        }
    }

    //fixup PolyNode links etc ...
    polytree.Childs.reserve(m_PolyOuts.size());
    for (OutRec &outRec : m_PolyOuts)
    {
        if (!outRec.PolyNd) continue;
        if (outRec.IsOpen) 
        {
          outRec.PolyNd->m_IsOpen = true;
          polytree.AddChild(*outRec.PolyNd);
        }
        else if (outRec.FirstLeft && outRec.FirstLeft->PolyNd) 
          outRec.FirstLeft->PolyNd->AddChild(*outRec.PolyNd);
        else
          polytree.AddChild(*outRec.PolyNd);
    }
}
//------------------------------------------------------------------------------

inline bool E2InsertsBeforeE1(TEdge &e1, TEdge &e2)
{
  if (e2.Curr.x() == e1.Curr.x()) 
  {
    if (e2.Top.y() > e1.Top.y())
      return e2.Top.x() < TopX(e1, e2.Top.y()); 
      else return e1.Top.x() > TopX(e2, e1.Top.y());
  } 
  else return e2.Curr.x() < e1.Curr.x();
}
//------------------------------------------------------------------------------

bool GetOverlap(const cInt a1, const cInt a2, const cInt b1, const cInt b2, 
    cInt& Left, cInt& Right)
{
  if (a1 < a2)
  {
    if (b1 < b2) {Left = std::max(a1,b1); Right = std::min(a2,b2);}
    else {Left = std::max(a1,b2); Right = std::min(a2,b1);}
  } 
  else
  {
    if (b1 < b2) {Left = std::max(a2,b1); Right = std::min(a1,b2);}
    else {Left = std::max(a2,b2); Right = std::min(a1,b1);}
  }
  return Left < Right;
}
//------------------------------------------------------------------------------

// Make all points of outrec point to outrec.Idx
inline void UpdateOutPtIdxs(OutRec& outrec)
{  
  OutPt* op = outrec.Pts;
  do
  {
    op->Idx = outrec.Idx;
    op = op->Prev;
  }
  while(op != outrec.Pts);
}
//------------------------------------------------------------------------------

void Clipper::InsertEdgeIntoAEL(TEdge *edge, TEdge* startEdge)
{
  if(!m_ActiveEdges)
  {
    edge->PrevInAEL = 0;
    edge->NextInAEL = 0;
    m_ActiveEdges = edge;
  }
  else if(!startEdge && E2InsertsBeforeE1(*m_ActiveEdges, *edge))
  {
      edge->PrevInAEL = 0;
      edge->NextInAEL = m_ActiveEdges;
      m_ActiveEdges->PrevInAEL = edge;
      m_ActiveEdges = edge;
  } 
  else
  {
    if(!startEdge) startEdge = m_ActiveEdges;
    while(startEdge->NextInAEL  && 
      !E2InsertsBeforeE1(*startEdge->NextInAEL , *edge))
        startEdge = startEdge->NextInAEL;
    edge->NextInAEL = startEdge->NextInAEL;
    if(startEdge->NextInAEL) startEdge->NextInAEL->PrevInAEL = edge;
    edge->PrevInAEL = startEdge;
    startEdge->NextInAEL = edge;
  }
}
//----------------------------------------------------------------------

OutPt* Clipper::DupOutPt(OutPt* outPt, bool InsertAfter)
{
  OutPt* result = this->AllocateOutPt();
  result->Pt = outPt->Pt;
  result->Idx = outPt->Idx;
  if (InsertAfter)
  {
    result->Next = outPt->Next;
    result->Prev = outPt;
    outPt->Next->Prev = result;
    outPt->Next = result;
  } 
  else
  {
    result->Prev = outPt->Prev;
    result->Next = outPt;
    outPt->Prev->Next = result;
    outPt->Prev = result;
  }
  return result;
}
//------------------------------------------------------------------------------

bool Clipper::JoinHorz(OutPt* op1, OutPt* op1b, OutPt* op2, OutPt* op2b,
  const IntPoint &Pt, bool DiscardLeft)
{
  Direction Dir1 = (op1->Pt.x() > op1b->Pt.x() ? dRightToLeft : dLeftToRight);
  Direction Dir2 = (op2->Pt.x() > op2b->Pt.x() ? dRightToLeft : dLeftToRight);
  if (Dir1 == Dir2) return false;

  //When DiscardLeft, we want Op1b to be on the Left of Op1, otherwise we
  //want Op1b to be on the Right. (And likewise with Op2 and Op2b.)
  //So, to facilitate this while inserting Op1b and Op2b ...
  //when DiscardLeft, make sure we're AT or RIGHT of Pt before adding Op1b,
  //otherwise make sure we're AT or LEFT of Pt. (Likewise with Op2b.)
  if (Dir1 == dLeftToRight) 
  {
    while (op1->Next->Pt.x() <= Pt.x() && 
      op1->Next->Pt.x() >= op1->Pt.x() && op1->Next->Pt.y() == Pt.y())  
        op1 = op1->Next;
    if (DiscardLeft && (op1->Pt.x() != Pt.x())) op1 = op1->Next;
    op1b = this->DupOutPt(op1, !DiscardLeft);
    if (op1b->Pt != Pt) 
    {
      op1 = op1b;
      op1->Pt = Pt;
      op1b = this->DupOutPt(op1, !DiscardLeft);
    }
  } 
  else
  {
    while (op1->Next->Pt.x() >= Pt.x() && 
      op1->Next->Pt.x() <= op1->Pt.x() && op1->Next->Pt.y() == Pt.y()) 
        op1 = op1->Next;
    if (!DiscardLeft && (op1->Pt.x() != Pt.x())) op1 = op1->Next;
    op1b = this->DupOutPt(op1, DiscardLeft);
    if (op1b->Pt != Pt)
    {
      op1 = op1b;
      op1->Pt = Pt;
      op1b = this->DupOutPt(op1, DiscardLeft);
    }
  }

  if (Dir2 == dLeftToRight)
  {
    while (op2->Next->Pt.x() <= Pt.x() && 
      op2->Next->Pt.x() >= op2->Pt.x() && op2->Next->Pt.y() == Pt.y())
        op2 = op2->Next;
    if (DiscardLeft && (op2->Pt.x() != Pt.x())) op2 = op2->Next;
    op2b = this->DupOutPt(op2, !DiscardLeft);
    if (op2b->Pt != Pt)
    {
      op2 = op2b;
      op2->Pt = Pt;
      op2b = this->DupOutPt(op2, !DiscardLeft);
    };
  } else
  {
    while (op2->Next->Pt.x() >= Pt.x() && 
      op2->Next->Pt.x() <= op2->Pt.x() && op2->Next->Pt.y() == Pt.y()) 
        op2 = op2->Next;
    if (!DiscardLeft && (op2->Pt.x() != Pt.x())) op2 = op2->Next;
    op2b = this->DupOutPt(op2, DiscardLeft);
    if (op2b->Pt != Pt)
    {
      op2 = op2b;
      op2->Pt = Pt;
      op2b = this->DupOutPt(op2, DiscardLeft);
    };
  };

  if ((Dir1 == dLeftToRight) == DiscardLeft)
  {
    op1->Prev = op2;
    op2->Next = op1;
    op1b->Next = op2b;
    op2b->Prev = op1b;
  }
  else
  {
    op1->Next = op2;
    op2->Prev = op1;
    op1b->Prev = op2b;
    op2b->Next = op1b;
  }
  return true;
}
//------------------------------------------------------------------------------

bool Clipper::JoinPoints(Join *j, OutRec* outRec1, OutRec* outRec2)
{
  OutPt *op1 = j->OutPt1, *op1b;
  OutPt *op2 = j->OutPt2, *op2b;

  //There are 3 kinds of joins for output polygons ...
  //1. Horizontal joins where Join.OutPt1 & Join.OutPt2 are vertices anywhere
  //along (horizontal) collinear edges (& Join.OffPt is on the same horizontal).
  //2. Non-horizontal joins where Join.OutPt1 & Join.OutPt2 are at the same
  //location at the Bottom of the overlapping segment (& Join.OffPt is above).
  //3. StrictSimple joins where edges touch but are not collinear and where
  //Join.OutPt1, Join.OutPt2 & Join.OffPt all share the same point.
  bool isHorizontal = (j->OutPt1->Pt.y() == j->OffPt.y());

  if (isHorizontal  && (j->OffPt == j->OutPt1->Pt) &&
  (j->OffPt == j->OutPt2->Pt))
  {
    //Strictly Simple join ...
    if (outRec1 != outRec2) return false;
    op1b = j->OutPt1->Next;
    while (op1b != op1 && (op1b->Pt == j->OffPt)) 
      op1b = op1b->Next;
    bool reverse1 = (op1b->Pt.y() > j->OffPt.y());
    op2b = j->OutPt2->Next;
    while (op2b != op2 && (op2b->Pt == j->OffPt)) 
      op2b = op2b->Next;
    bool reverse2 = (op2b->Pt.y() > j->OffPt.y());
    if (reverse1 == reverse2) return false;
    if (reverse1)
    {
      op1b = this->DupOutPt(op1, false);
      op2b = this->DupOutPt(op2, true);
      op1->Prev = op2;
      op2->Next = op1;
      op1b->Next = op2b;
      op2b->Prev = op1b;
      j->OutPt1 = op1;
      j->OutPt2 = op1b;
      return true;
    } else
    {
      op1b = this->DupOutPt(op1, true);
      op2b = this->DupOutPt(op2, false);
      op1->Next = op2;
      op2->Prev = op1;
      op1b->Prev = op2b;
      op2b->Next = op1b;
      j->OutPt1 = op1;
      j->OutPt2 = op1b;
      return true;
    }
  } 
  else if (isHorizontal)
  {
    //treat horizontal joins differently to non-horizontal joins since with
    //them we're not yet sure where the overlapping is. OutPt1.Pt & OutPt2.Pt
    //may be anywhere along the horizontal edge.
    op1b = op1;
    while (op1->Prev->Pt.y() == op1->Pt.y() && op1->Prev != op1b && op1->Prev != op2)
      op1 = op1->Prev;
    while (op1b->Next->Pt.y() == op1b->Pt.y() && op1b->Next != op1 && op1b->Next != op2)
      op1b = op1b->Next;
    if (op1b->Next == op1 || op1b->Next == op2) return false; //a flat 'polygon'

    op2b = op2;
    while (op2->Prev->Pt.y() == op2->Pt.y() && op2->Prev != op2b && op2->Prev != op1b)
      op2 = op2->Prev;
    while (op2b->Next->Pt.y() == op2b->Pt.y() && op2b->Next != op2 && op2b->Next != op1)
      op2b = op2b->Next;
    if (op2b->Next == op2 || op2b->Next == op1) return false; //a flat 'polygon'

    cInt Left, Right;
    //Op1 --> Op1b & Op2 --> Op2b are the extremites of the horizontal edges
    if (!GetOverlap(op1->Pt.x(), op1b->Pt.x(), op2->Pt.x(), op2b->Pt.x(), Left, Right))
      return false;

    //DiscardLeftSide: when overlapping edges are joined, a spike will created
    //which needs to be cleaned up. However, we don't want Op1 or Op2 caught up
    //on the discard Side as either may still be needed for other joins ...
    IntPoint Pt;
    bool DiscardLeftSide;
    if (op1->Pt.x() >= Left && op1->Pt.x() <= Right) 
    {
      Pt = op1->Pt; DiscardLeftSide = (op1->Pt.x() > op1b->Pt.x());
    } 
    else if (op2->Pt.x() >= Left&& op2->Pt.x() <= Right) 
    {
      Pt = op2->Pt; DiscardLeftSide = (op2->Pt.x() > op2b->Pt.x());
    } 
    else if (op1b->Pt.x() >= Left && op1b->Pt.x() <= Right)
    {
      Pt = op1b->Pt; DiscardLeftSide = op1b->Pt.x() > op1->Pt.x();
    } 
    else
    {
      Pt = op2b->Pt; DiscardLeftSide = (op2b->Pt.x() > op2->Pt.x());
    }
    j->OutPt1 = op1; j->OutPt2 = op2;
    return JoinHorz(op1, op1b, op2, op2b, Pt, DiscardLeftSide);
  } else
  {
    //nb: For non-horizontal joins ...
    //    1. Jr.OutPt1.Pt.y() == Jr.OutPt2.Pt.y()
    //    2. Jr.OutPt1.Pt > Jr.OffPt.y()

    //make sure the polygons are correctly oriented ...
    op1b = op1->Next;
    while ((op1b->Pt == op1->Pt) && (op1b != op1)) op1b = op1b->Next;
    bool Reverse1 = ((op1b->Pt.y() > op1->Pt.y()) ||
      !SlopesEqual(op1->Pt, op1b->Pt, j->OffPt, m_UseFullRange));
    if (Reverse1)
    {
      op1b = op1->Prev;
      while ((op1b->Pt == op1->Pt) && (op1b != op1)) op1b = op1b->Prev;
      if ((op1b->Pt.y() > op1->Pt.y()) ||
        !SlopesEqual(op1->Pt, op1b->Pt, j->OffPt, m_UseFullRange)) return false;
    };
    op2b = op2->Next;
    while ((op2b->Pt == op2->Pt) && (op2b != op2))op2b = op2b->Next;
    bool Reverse2 = ((op2b->Pt.y() > op2->Pt.y()) ||
      !SlopesEqual(op2->Pt, op2b->Pt, j->OffPt, m_UseFullRange));
    if (Reverse2)
    {
      op2b = op2->Prev;
      while ((op2b->Pt == op2->Pt) && (op2b != op2)) op2b = op2b->Prev;
      if ((op2b->Pt.y() > op2->Pt.y()) ||
        !SlopesEqual(op2->Pt, op2b->Pt, j->OffPt, m_UseFullRange)) return false;
    }

    if ((op1b == op1) || (op2b == op2) || (op1b == op2b) ||
      ((outRec1 == outRec2) && (Reverse1 == Reverse2))) return false;

    if (Reverse1)
    {
      op1b = this->DupOutPt(op1, false);
      op2b = this->DupOutPt(op2, true);
      op1->Prev = op2;
      op2->Next = op1;
      op1b->Next = op2b;
      op2b->Prev = op1b;
      j->OutPt1 = op1;
      j->OutPt2 = op1b;
      return true;
    } else
    {
      op1b = this->DupOutPt(op1, true);
      op2b = this->DupOutPt(op2, false);
      op1->Next = op2;
      op2->Prev = op1;
      op1b->Prev = op2b;
      op2b->Next = op1b;
      j->OutPt1 = op1;
      j->OutPt2 = op1b;
      return true;
    }
  }
}
//----------------------------------------------------------------------

inline OutRec* ParseFirstLeft(OutRec* FirstLeft)
{
  while (FirstLeft && !FirstLeft->Pts)
    FirstLeft = FirstLeft->FirstLeft;
  return FirstLeft;
}
//------------------------------------------------------------------------------

// This is potentially very expensive! O(n^3)!
void Clipper::FixupFirstLefts1(OutRec* OldOutRec, OutRec* NewOutRec)
{ 
  //tests if NewOutRec contains the polygon before reassigning FirstLeft
  for (OutRec &outRec : m_PolyOuts)
  {
    OutRec* firstLeft = ParseFirstLeft(outRec.FirstLeft);
    if (outRec.Pts && firstLeft == OldOutRec && Poly2ContainsPoly1(outRec.Pts, NewOutRec->Pts))
        outRec.FirstLeft = NewOutRec;
  }
}
//----------------------------------------------------------------------

void Clipper::FixupFirstLefts2(OutRec* InnerOutRec, OutRec* OuterOutRec)
{
  //A polygon has split into two such that one is now the inner of the other.
  //It's possible that these polygons now wrap around other polygons, so check
  //every polygon that's also contained by OuterOutRec's FirstLeft container
  //(including 0) to see if they've become inner to the new inner polygon ...
  OutRec* orfl = OuterOutRec->FirstLeft;
  for (OutRec &outRec : m_PolyOuts)
  {
    if (!outRec.Pts || &outRec == OuterOutRec || &outRec == InnerOutRec)
      continue;
    OutRec* firstLeft = ParseFirstLeft(outRec.FirstLeft);
    if (firstLeft != orfl && firstLeft != InnerOutRec && firstLeft != OuterOutRec)
      continue;
    if (Poly2ContainsPoly1(outRec.Pts, InnerOutRec->Pts))
      outRec.FirstLeft = InnerOutRec;
    else if (Poly2ContainsPoly1(outRec.Pts, OuterOutRec->Pts))
      outRec.FirstLeft = OuterOutRec;
    else if (outRec.FirstLeft == InnerOutRec || outRec.FirstLeft == OuterOutRec)
      outRec.FirstLeft = orfl;
  }
}
//----------------------------------------------------------------------
void Clipper::FixupFirstLefts3(OutRec* OldOutRec, OutRec* NewOutRec)
{
  //reassigns FirstLeft WITHOUT testing if NewOutRec contains the polygon
  for (OutRec &outRec : m_PolyOuts)
  {
    OutRec* firstLeft = ParseFirstLeft(outRec.FirstLeft);
    if (outRec.Pts && firstLeft == OldOutRec)
      outRec.FirstLeft = NewOutRec;
  }
}
//----------------------------------------------------------------------

void Clipper::JoinCommonEdges()
{
  for (Join &join : m_Joins)
  {
    OutRec *outRec1 = GetOutRec(join.OutPt1->Idx);
    OutRec *outRec2 = GetOutRec(join.OutPt2->Idx);

    if (!outRec1->Pts || !outRec2->Pts) continue;
    if (outRec1->IsOpen || outRec2->IsOpen) continue;

    //get the polygon fragment with the correct hole state (FirstLeft)
    //before calling JoinPoints() ...
    OutRec *holeStateRec;
    if (outRec1 == outRec2) holeStateRec = outRec1;
    else if (OutRec1RightOfOutRec2(outRec1, outRec2)) holeStateRec = outRec2;
    else if (OutRec1RightOfOutRec2(outRec2, outRec1)) holeStateRec = outRec1;
    else holeStateRec = GetLowermostRec(outRec1, outRec2);

    if (!JoinPoints(&join, outRec1, outRec2)) continue;

    if (outRec1 == outRec2)
    {
      //instead of joining two polygons, we've just created a new one by
      //splitting one polygon into two.
      outRec1->Pts = join.OutPt1;
      outRec1->BottomPt = 0;
      outRec2 = CreateOutRec();
      outRec2->Pts = join.OutPt2;

      //update all OutRec2.Pts Idx's ...
      UpdateOutPtIdxs(*outRec2);

      if (Poly2ContainsPoly1(outRec2->Pts, outRec1->Pts))
      {
        //outRec1 contains outRec2 ...
        outRec2->IsHole = !outRec1->IsHole;
        outRec2->FirstLeft = outRec1;

        // For each m_PolyOuts, replace FirstLeft from outRec2 to outRec1.
        if (m_UsingPolyTree) FixupFirstLefts2(outRec2, outRec1);

        if ((outRec2->IsHole ^ m_ReverseOutput) == (Area(*outRec2) > 0))
          ReversePolyPtLinks(outRec2->Pts);
            
      } else if (Poly2ContainsPoly1(outRec1->Pts, outRec2->Pts))
      {
        //outRec2 contains outRec1 ...
        outRec2->IsHole = outRec1->IsHole;
        outRec1->IsHole = !outRec2->IsHole;
        outRec2->FirstLeft = outRec1->FirstLeft;
        outRec1->FirstLeft = outRec2;

        // For each m_PolyOuts, replace FirstLeft from outRec1 to outRec2.
        if (m_UsingPolyTree) FixupFirstLefts2(outRec1, outRec2);

        if ((outRec1->IsHole ^ m_ReverseOutput) == (Area(*outRec1) > 0))
          ReversePolyPtLinks(outRec1->Pts);
      } 
      else
      {
        //the 2 polygons are completely separate ...
        outRec2->IsHole = outRec1->IsHole;
        outRec2->FirstLeft = outRec1->FirstLeft;

        //fixup FirstLeft pointers that may need reassigning to OutRec2
        // For each polygon of m_PolyOuts, replace FirstLeft from outRec1 to outRec2 if the polygon is inside outRec2.
        //FIXME This is potentially very expensive! O(n^3)!
        if (m_UsingPolyTree) FixupFirstLefts1(outRec1, outRec2);
      }
     
    } else
    {
      //joined 2 polygons together ...

      outRec2->Pts = 0;
      outRec2->BottomPt = 0;
      outRec2->Idx = outRec1->Idx;

      outRec1->IsHole = holeStateRec->IsHole;
      if (holeStateRec == outRec2) 
        outRec1->FirstLeft = outRec2->FirstLeft;
      outRec2->FirstLeft = outRec1;

      // For each m_PolyOuts, replace FirstLeft from outRec2 to outRec1.
      if (m_UsingPolyTree) FixupFirstLefts3(outRec2, outRec1);
    }
  }
}

//------------------------------------------------------------------------------
// ClipperOffset support functions ...
//------------------------------------------------------------------------------

DoublePoint GetUnitNormal(const IntPoint &pt1, const IntPoint &pt2)
{
  if(pt2.x() == pt1.x() && pt2.y() == pt1.y()) 
    return DoublePoint(0, 0);

  double Dx = double(pt2.x() - pt1.x());
  double dy = double(pt2.y() - pt1.y());
  double f = 1.0 / std::sqrt( Dx*Dx + dy*dy );
  Dx *= f;
  dy *= f;
  return DoublePoint(dy, -Dx);
}

//------------------------------------------------------------------------------
// ClipperOffset class
//------------------------------------------------------------------------------

void ClipperOffset::Clear()
{
  for (int i = 0; i < m_polyNodes.ChildCount(); ++i)
    delete m_polyNodes.Childs[i];
  m_polyNodes.Childs.clear();
  m_lowest.x() = -1;
}
//------------------------------------------------------------------------------

void ClipperOffset::AddPath(const Path& path, JoinType joinType, EndType endType)
{
  int highI = (int)path.size() - 1;
  if (highI < 0) return;
  PolyNode* newNode = new PolyNode();
  newNode->m_jointype = joinType;
  newNode->m_endtype = endType;

  //strip duplicate points from path and also get index to the lowest point ...
  bool   has_shortest_edge_length = ShortestEdgeLength > 0.;
  double shortest_edge_length2 = has_shortest_edge_length ? ShortestEdgeLength * ShortestEdgeLength : 0.;
  if (endType == etClosedLine || endType == etClosedPolygon)
    for (; highI > 0; -- highI) {
      bool same = false;
      if (has_shortest_edge_length) {
        double dx = double(path[highI].x() - path[0].x());
        double dy = double(path[highI].y() - path[0].y());
        same = dx*dx + dy*dy < shortest_edge_length2;
      } else
        same = path[0] == path[highI];
      if (! same)
        break;
    }
  newNode->Contour.reserve(highI + 1);
  newNode->Contour.emplace_back(path[0]);
  int j = 0, k = 0;
  for (int i = 1; i <= highI; i++) {
    bool same = false;
    if (has_shortest_edge_length) {
      double dx = double(path[i].x() - newNode->Contour[j].x());
      double dy = double(path[i].y() - newNode->Contour[j].y());
      same = dx*dx + dy*dy < shortest_edge_length2;
    } else
      same = newNode->Contour[j] == path[i];
    if (same)
      continue;
    j++;
    newNode->Contour.emplace_back(path[i]);
    if (path[i].y() > newNode->Contour[k].y() ||
      (path[i].y() == newNode->Contour[k].y() &&
      path[i].x() < newNode->Contour[k].x())) k = j;
  }
  if (endType == etClosedPolygon && j < 2)
  {
    delete newNode;
    return;
  }
  m_polyNodes.AddChild(*newNode);

  //if this path's lowest pt is lower than all the others then update m_lowest
  if (endType != etClosedPolygon) return;
  if (m_lowest.x() < 0)
    m_lowest = IntPoint2d(m_polyNodes.ChildCount() - 1, k);
  else
  {
    IntPoint ip = m_polyNodes.Childs[(int)m_lowest.x()]->Contour[(int)m_lowest.y()];
    if (newNode->Contour[k].y() > ip.y() ||
      (newNode->Contour[k].y() == ip.y() &&
      newNode->Contour[k].x() < ip.x()))
      m_lowest = IntPoint2d(m_polyNodes.ChildCount() - 1, k);
  }
}
//------------------------------------------------------------------------------

void ClipperOffset::FixOrientations()
{
  //fixup orientations of all closed paths if the orientation of the
  //closed path with the lowermost vertex is wrong ...
  if (m_lowest.x() >= 0 && 
    !Orientation(m_polyNodes.Childs[(int)m_lowest.x()]->Contour))
  {
    for (int i = 0; i < m_polyNodes.ChildCount(); ++i)
    {
      PolyNode& node = *m_polyNodes.Childs[i];
      if (node.m_endtype == etClosedPolygon ||
        (node.m_endtype == etClosedLine && Orientation(node.Contour)))
          ReversePath(node.Contour);
    }
  } else
  {
    for (int i = 0; i < m_polyNodes.ChildCount(); ++i)
    {
      PolyNode& node = *m_polyNodes.Childs[i];
      if (node.m_endtype == etClosedLine && !Orientation(node.Contour))
        ReversePath(node.Contour);
    }
  }
}
//------------------------------------------------------------------------------

void ClipperOffset::Execute(Paths& solution, double delta)
{
  solution.clear();
  FixOrientations();
  DoOffset(delta);
  
  //now clean up 'corners' ...
  Clipper clpr;
  clpr.AddPaths(m_destPolys, ptSubject, true);
  if (delta > 0)
  {
    clpr.Execute(ctUnion, solution, pftPositive, pftPositive);
  }
  else
  {
    IntRect r = clpr.GetBounds();
    Path outer(4);
    outer[0] = IntPoint2d(r.left - 10, r.bottom + 10);
    outer[1] = IntPoint2d(r.right + 10, r.bottom + 10);
    outer[2] = IntPoint2d(r.right + 10, r.top - 10);
    outer[3] = IntPoint2d(r.left - 10, r.top - 10);

    clpr.AddPath(outer, ptSubject, true);
    clpr.ReverseSolution(true);
    clpr.Execute(ctUnion, solution, pftNegative, pftNegative);
    if (! solution.empty())
      solution.erase(solution.begin());
  }
}
//------------------------------------------------------------------------------

void ClipperOffset::Execute(PolyTree& solution, double delta)
{
  solution.Clear();
  FixOrientations();
  DoOffset(delta);

  //now clean up 'corners' ...
  Clipper clpr;
  clpr.AddPaths(m_destPolys, ptSubject, true);
  if (delta > 0)
  {
    clpr.Execute(ctUnion, solution, pftPositive, pftPositive);
  }
  else
  {
    IntRect r = clpr.GetBounds();
    Path outer(4);
    outer[0] = IntPoint2d(r.left - 10, r.bottom + 10);
    outer[1] = IntPoint2d(r.right + 10, r.bottom + 10);
    outer[2] = IntPoint2d(r.right + 10, r.top - 10);
    outer[3] = IntPoint2d(r.left - 10, r.top - 10);

    clpr.AddPath(outer, ptSubject, true);
    clpr.ReverseSolution(true);
    clpr.Execute(ctUnion, solution, pftNegative, pftNegative);
    //remove the outer PolyNode rectangle ...
    solution.RemoveOutermostPolygon();
  }
}
//------------------------------------------------------------------------------

void ClipperOffset::DoOffset(double delta)
{
  m_destPolys.clear();
  m_delta = delta;

  //if Zero offset, just copy any CLOSED polygons to m_p and return ...
  if (NEAR_ZERO(delta)) 
  {
    m_destPolys.reserve(m_polyNodes.ChildCount());
    for (int i = 0; i < m_polyNodes.ChildCount(); i++)
    {
      PolyNode& node = *m_polyNodes.Childs[i];
      if (node.m_endtype == etClosedPolygon)
        m_destPolys.emplace_back(node.Contour);
    }
    return;
  }

  //see offset_triginometry3.svg in the documentation folder ...
  m_miterLim = (MiterLimit > 2) ? 
    2. / (MiterLimit * MiterLimit) :
    0.5;

  double y;
  if (ArcTolerance <= 0.0) y = def_arc_tolerance;
  else if (ArcTolerance > std::fabs(delta) * def_arc_tolerance) 
    y = std::fabs(delta) * def_arc_tolerance;
  else y = ArcTolerance;
  //see offset_triginometry2.svg in the documentation folder ...
  double steps = pi / std::acos(1 - y / std::fabs(delta));
  if (steps > std::fabs(delta) * pi) 
    steps = std::fabs(delta) * pi;  //ie excessive precision check
  m_sin = std::sin(two_pi / steps);
  m_cos = std::cos(two_pi / steps);
  m_StepsPerRad = steps / two_pi;
  if (delta < 0.0) m_sin = -m_sin;

  m_destPolys.reserve(m_polyNodes.ChildCount() * 2);
  for (int i = 0; i < m_polyNodes.ChildCount(); i++)
  {
    PolyNode& node = *m_polyNodes.Childs[i];
    m_srcPoly = node.Contour;

    int len = (int)m_srcPoly.size();
    if (len == 0 || (delta <= 0 && (len < 3 || node.m_endtype != etClosedPolygon)))
        continue;

    m_destPoly.clear();
    if (len == 1)
    {
      if (node.m_jointype == jtRound)
      {
        double X = 1.0, Y = 0.0;
        for (cInt j = 1; j <= steps; j++)
        {
          m_destPoly.emplace_back(IntPoint2d(
            Round<cInt>(m_srcPoly[0].x() + X * delta),
            Round<cInt>(m_srcPoly[0].y() + Y * delta)));
          double X2 = X;
          X = X * m_cos - m_sin * Y;
          Y = X2 * m_sin + Y * m_cos;
        }
      }
      else
      {
        double X = -1.0, Y = -1.0;
        for (int j = 0; j < 4; ++j)
        {
          m_destPoly.emplace_back(IntPoint2d(
            Round<cInt>(m_srcPoly[0].x() + X * delta),
            Round<cInt>(m_srcPoly[0].y() + Y * delta)));
          if (X < 0) X = 1;
          else if (Y < 0) Y = 1;
          else X = -1;
        }
      }
      m_destPolys.emplace_back(m_destPoly);
      continue;
    }
    //build m_normals ...
    m_normals.clear();
    m_normals.reserve(len);
    for (int j = 0; j < len - 1; ++j)
      m_normals.emplace_back(GetUnitNormal(m_srcPoly[j], m_srcPoly[j + 1]));
    if (node.m_endtype == etClosedLine || node.m_endtype == etClosedPolygon)
      m_normals.emplace_back(GetUnitNormal(m_srcPoly[len - 1], m_srcPoly[0]));
    else
      m_normals.emplace_back(DoublePoint(m_normals[len - 2]));

    if (node.m_endtype == etClosedPolygon)
    {
      int k = len - 1;
      for (int j = 0; j < len; ++j)
        OffsetPoint(j, k, node.m_jointype);
      m_destPolys.emplace_back(m_destPoly);
    }
    else if (node.m_endtype == etClosedLine)
    {
      int k = len - 1;
      for (int j = 0; j < len; ++j)
        OffsetPoint(j, k, node.m_jointype);
      m_destPolys.emplace_back(m_destPoly);
      m_destPoly.clear();
      //re-build m_normals ...
      DoublePoint n = m_normals[len -1];
      for (int j = len - 1; j > 0; j--)
        m_normals[j] = DoublePoint(-m_normals[j - 1].x(), -m_normals[j - 1].y());
      m_normals[0] = DoublePoint(-n.x(), -n.y());
      k = 0;
      for (int j = len - 1; j >= 0; j--)
        OffsetPoint(j, k, node.m_jointype);
      m_destPolys.emplace_back(m_destPoly);
    }
    else
    {
      int k = 0;
      for (int j = 1; j < len - 1; ++j)
        OffsetPoint(j, k, node.m_jointype);

      IntPoint pt1;
      if (node.m_endtype == etOpenButt)
      {
        int j = len - 1;
        pt1 = IntPoint2d(Round<cInt>(m_srcPoly[j].x() + m_normals[j].x() * delta), Round<cInt>(m_srcPoly[j].y() + m_normals[j].y() * delta));
        m_destPoly.emplace_back(pt1);
        pt1 = IntPoint2d(Round<cInt>(m_srcPoly[j].x() - m_normals[j].x() * delta), Round<cInt>(m_srcPoly[j].y() - m_normals[j].y() * delta));
        m_destPoly.emplace_back(pt1);
      }
      else
      {
        int j = len - 1;
        k = len - 2;
        m_sinA = 0;
        m_normals[j] = DoublePoint(-m_normals[j].x(), -m_normals[j].y());
        if (node.m_endtype == etOpenSquare)
          DoSquare(j, k);
        else
          DoRound(j, k);
      }

      //re-build m_normals ...
      for (int j = len - 1; j > 0; j--)
        m_normals[j] = DoublePoint(-m_normals[j - 1].x(), -m_normals[j - 1].y());
      m_normals[0] = DoublePoint(-m_normals[1].x(), -m_normals[1].y());

      k = len - 1;
      for (int j = k - 1; j > 0; --j) OffsetPoint(j, k, node.m_jointype);

      if (node.m_endtype == etOpenButt)
      {
        pt1 = IntPoint2d(Round<cInt>(m_srcPoly[0].x() - m_normals[0].x() * delta), Round<cInt>(m_srcPoly[0].y() - m_normals[0].y() * delta));
        m_destPoly.emplace_back(pt1);
        pt1 = IntPoint2d(Round<cInt>(m_srcPoly[0].x() + m_normals[0].x() * delta), Round<cInt>(m_srcPoly[0].y() + m_normals[0].y() * delta));
        m_destPoly.emplace_back(pt1);
      }
      else
      {
        k = 1;
        m_sinA = 0;
        if (node.m_endtype == etOpenSquare)
          DoSquare(0, 1);
        else
          DoRound(0, 1);
      }
      m_destPolys.emplace_back(m_destPoly);
    }
  }
}
//------------------------------------------------------------------------------

void ClipperOffset::OffsetPoint(int j, int& k, JoinType jointype)
{
  //cross product ...
  m_sinA = (m_normals[k].x() * m_normals[j].y() - m_normals[j].x() * m_normals[k].y());
  if (std::fabs(m_sinA * m_delta) < 1.0) 
  {
    //dot product ...
    double cosA = (m_normals[k].x() * m_normals[j].x() + m_normals[j].y() * m_normals[k].y() ); 
    if (cosA > 0) // angle => 0 degrees
    {
      m_destPoly.emplace_back(IntPoint2d(Round<cInt>(m_srcPoly[j].x() + m_normals[k].x() * m_delta),
        Round<cInt>(m_srcPoly[j].y() + m_normals[k].y() * m_delta)));
      return; 
    }
    //else angle => 180 degrees   
  }
  else if (m_sinA > 1.0) m_sinA = 1.0;
  else if (m_sinA < -1.0) m_sinA = -1.0;

  if (m_sinA * m_delta < 0)
  {
    m_destPoly.emplace_back(IntPoint2d(Round<cInt>(m_srcPoly[j].x() + m_normals[k].x() * m_delta),
      Round<cInt>(m_srcPoly[j].y() + m_normals[k].y() * m_delta)));
    m_destPoly.emplace_back(m_srcPoly[j]);
    m_destPoly.emplace_back(IntPoint2d(Round<cInt>(m_srcPoly[j].x() + m_normals[j].x() * m_delta),
      Round<cInt>(m_srcPoly[j].y() + m_normals[j].y() * m_delta)));
  }
  else
    switch (jointype)
    {
      case jtMiter:
        {
          double r = 1 + (m_normals[j].x() * m_normals[k].x() +
            m_normals[j].y() * m_normals[k].y());
          if (r >= m_miterLim) DoMiter(j, k, r); else DoSquare(j, k);
          break;
        }
      case jtSquare: DoSquare(j, k); break;
      case jtRound: DoRound(j, k); break;
    }
  k = j;
}
//------------------------------------------------------------------------------

void ClipperOffset::DoSquare(int j, int k)
{
  double dx = std::tan(std::atan2(m_sinA,
      m_normals[k].x() * m_normals[j].x() + m_normals[k].y() * m_normals[j].y()) / 4);
  m_destPoly.emplace_back(IntPoint2d(
      Round<cInt>(m_srcPoly[j].x() + m_delta * (m_normals[k].x() - m_normals[k].y() * dx)),
      Round<cInt>(m_srcPoly[j].y() + m_delta * (m_normals[k].y() + m_normals[k].x() * dx))));
  m_destPoly.emplace_back(IntPoint2d(
      Round<cInt>(m_srcPoly[j].x() + m_delta * (m_normals[j].x() + m_normals[j].y() * dx)),
      Round<cInt>(m_srcPoly[j].y() + m_delta * (m_normals[j].y() - m_normals[j].x() * dx))));
}
//------------------------------------------------------------------------------

void ClipperOffset::DoMiter(int j, int k, double r)
{
  double q = m_delta / r;
  m_destPoly.emplace_back(IntPoint2d(Round<cInt>(m_srcPoly[j].x() + (m_normals[k].x() + m_normals[j].x()) * q),
      Round<cInt>(m_srcPoly[j].y() + (m_normals[k].y() + m_normals[j].y()) * q)));
}
//------------------------------------------------------------------------------

void ClipperOffset::DoRound(int j, int k)
{
  double a = std::atan2(m_sinA,
  m_normals[k].x() * m_normals[j].x() + m_normals[k].y() * m_normals[j].y());
  auto steps = std::max<int>(Round<cInt>(m_StepsPerRad * std::fabs(a)), 1);

  double X = m_normals[k].x(), Y = m_normals[k].y(), X2;
  for (int i = 0; i < steps; ++i)
  {
    m_destPoly.emplace_back(IntPoint2d(
        Round<cInt>(m_srcPoly[j].x() + X * m_delta),
        Round<cInt>(m_srcPoly[j].y() + Y * m_delta)));
    X2 = X;
    X = X * m_cos - m_sin * Y;
    Y = X2 * m_sin + Y * m_cos;
  }
  m_destPoly.emplace_back(IntPoint2d(
  Round<cInt>(m_srcPoly[j].x() + m_normals[j].x() * m_delta),
  Round<cInt>(m_srcPoly[j].y() + m_normals[j].y() * m_delta)));
}

//------------------------------------------------------------------------------
// Miscellaneous public functions
//------------------------------------------------------------------------------

// Called by Clipper::ExecuteInternal()
// For each polygon, search for exactly duplicate non-successive points.
// If such a point is found, the loop is split into two pieces.
// Search for the duplicate points is O(n^2)!
// http://www.angusj.com/delphi/clipper/documentation/Docs/Units/ClipperLib/Classes/Clipper/Properties/StrictlySimple.htm
void Clipper::DoSimplePolygons()
{
  size_t i = 0;
  while (i < m_PolyOuts.size()) 
  {
    OutRec &outrec = m_PolyOuts[i++];
    OutPt* op = outrec.Pts;
    if (!op || outrec.IsOpen) continue;
    do //for each Pt in Polygon until duplicate found do ...
    {
      OutPt* op2 = op->Next;
      while (op2 != outrec.Pts) 
      {
        if ((op->Pt == op2->Pt) && op2->Next != op && op2->Prev != op) 
        {
          //split the polygon into two ...
          OutPt* op3 = op->Prev;
          OutPt* op4 = op2->Prev;
          op->Prev = op4;
          op4->Next = op;
          op2->Prev = op3;
          op3->Next = op2;

          outrec.Pts = op;
          OutRec* outrec2 = CreateOutRec();
          outrec2->Pts = op2;
          UpdateOutPtIdxs(*outrec2);
          if (Poly2ContainsPoly1(outrec2->Pts, outrec.Pts))
          {
            //OutRec2 is contained by OutRec1 ...
            outrec2->IsHole = !outrec.IsHole;
            outrec2->FirstLeft = &outrec;
            // For each m_PolyOuts, replace FirstLeft from outRec2 to outrec.
            if (m_UsingPolyTree) FixupFirstLefts2(outrec2, &outrec);
          }
          else
            if (Poly2ContainsPoly1(outrec.Pts, outrec2->Pts))
          {
            //OutRec1 is contained by OutRec2 ...
            outrec2->IsHole = outrec.IsHole;
            outrec.IsHole = !outrec2->IsHole;
            outrec2->FirstLeft = outrec.FirstLeft;
            outrec.FirstLeft = outrec2;
            // For each m_PolyOuts, replace FirstLeft from outrec to outrec2.
            if (m_UsingPolyTree) FixupFirstLefts2(&outrec, outrec2);
            }
            else
          {
            //the 2 polygons are separate ...
            outrec2->IsHole = outrec.IsHole;
            outrec2->FirstLeft = outrec.FirstLeft;
            // For each polygon of m_PolyOuts, replace FirstLeft from outrec to outrec2 if the polygon is inside outRec2.
            //FIXME This is potentially very expensive! O(n^3)!
            if (m_UsingPolyTree) FixupFirstLefts1(&outrec, outrec2);
          }
          op2 = op; //ie get ready for the Next iteration
        }
        op2 = op2->Next;
      }
      op = op->Next;
    }
    while (op != outrec.Pts);
  }
}
//------------------------------------------------------------------------------

void ReversePath(Path& p)
{
  std::reverse(p.begin(), p.end());
}
//------------------------------------------------------------------------------

void ReversePaths(Paths& p)
{
  for (Paths::size_type i = 0; i < p.size(); ++i)
    ReversePath(p[i]);
}
//------------------------------------------------------------------------------

Paths SimplifyPolygon(const Path &in_poly, PolyFillType fillType, bool strictly_simple /* = true */)
{
  Clipper c;
  c.StrictlySimple(strictly_simple);
  c.AddPath(in_poly, ptSubject, true);
  Paths out; 
  c.Execute(ctUnion, out, fillType, fillType);
  return out;
}

//------------------------------------------------------------------------------

inline double DistanceSqrd(const IntPoint& pt1, const IntPoint& pt2)
{
  auto Dx = double(pt1.x() - pt2.x());
  auto dy = double(pt1.y() - pt2.y());
  return (Dx*Dx + dy*dy);
}
//------------------------------------------------------------------------------

double DistanceFromLineSqrd(
  const IntPoint& pt, const IntPoint& ln1, const IntPoint& ln2)
{
  //The equation of a line in general form (Ax + By + C = 0)
  //given 2 points (x¹,y¹) & (x²,y²) is ...
  //(y¹ - y²)x + (x² - x¹)y + (y² - y¹)x¹ - (x² - x¹)y¹ = 0
  //A = (y¹ - y²); B = (x² - x¹); C = (y² - y¹)x¹ - (x² - x¹)y¹
  //perpendicular distance of point (x³,y³) = (Ax³ + By³ + C)/Sqrt(A² + B²)
  //see http://en.wikipedia.org/wiki/Perpendicular_distance
  double A = double(ln1.y() - ln2.y());
  double B = double(ln2.x() - ln1.x());
  double C = A * ln1.x()  + B * ln1.y();
  C = A * pt.x() + B * pt.y() - C;
  return (C * C) / (A * A + B * B);
}
//---------------------------------------------------------------------------

bool SlopesNearCollinear(const IntPoint& pt1, 
    const IntPoint& pt2, const IntPoint& pt3, double distSqrd)
{
  //this function is more accurate when the point that's geometrically
  //between the other 2 points is the one that's tested for distance.
  //ie makes it more likely to pick up 'spikes' ...
	if (std::abs(pt1.x() - pt2.x()) > std::abs(pt1.y() - pt2.y()))
	{
    if ((pt1.x() > pt2.x()) == (pt1.x() < pt3.x()))
      return DistanceFromLineSqrd(pt1, pt2, pt3) < distSqrd;
    else if ((pt2.x() > pt1.x()) == (pt2.x() < pt3.x()))
      return DistanceFromLineSqrd(pt2, pt1, pt3) < distSqrd;
		else
	    return DistanceFromLineSqrd(pt3, pt1, pt2) < distSqrd;
	}
	else
	{
    if ((pt1.y() > pt2.y()) == (pt1.y() < pt3.y()))
      return DistanceFromLineSqrd(pt1, pt2, pt3) < distSqrd;
    else if ((pt2.y() > pt1.y()) == (pt2.y() < pt3.y()))
      return DistanceFromLineSqrd(pt2, pt1, pt3) < distSqrd;
		else
      return DistanceFromLineSqrd(pt3, pt1, pt2) < distSqrd;
	}
}
//------------------------------------------------------------------------------

bool PointsAreClose(IntPoint pt1, IntPoint pt2, double distSqrd)
{
    auto Dx = double(pt1.x() - pt2.x());
    auto dy = double(pt1.y() - pt2.y());
    return ((Dx * Dx) + (dy * dy) <= distSqrd);
}
//------------------------------------------------------------------------------

OutPt* ExcludeOp(OutPt* op)
{
  OutPt* result = op->Prev;
  result->Next = op->Next;
  op->Next->Prev = result;
  result->Idx = 0;
  return result;
}
//------------------------------------------------------------------------------

// Simplify a polygon using a linked list of points.
void CleanPolygon(const Path& in_poly, Path& out_poly, double distance)
{
  //distance = proximity in units/pixels below which vertices
  //will be stripped. Default ~= sqrt(2).
  
  size_t size = in_poly.size();
  
  if (size == 0) 
  {
    out_poly.clear();
    return;
  }

  OutPts outPts(size);
  for (size_t i = 0; i < size; ++i)
  {
    outPts[i].Pt = in_poly[i];
    outPts[i].Next = &outPts[(i + 1) % size];
    outPts[i].Next->Prev = &outPts[i];
    outPts[i].Idx = 0;
  }

  double distSqrd = distance * distance;
  OutPt* op = &outPts[0];
  while (op->Idx == 0 && op->Next != op->Prev) 
  {
    if (PointsAreClose(op->Pt, op->Prev->Pt, distSqrd))
    {
      op = ExcludeOp(op);
      size--;
    } 
    else if (PointsAreClose(op->Prev->Pt, op->Next->Pt, distSqrd))
    {
      ExcludeOp(op->Next);
      op = ExcludeOp(op);
      size -= 2;
    }
    else if (SlopesNearCollinear(op->Prev->Pt, op->Pt, op->Next->Pt, distSqrd))
    {
      op = ExcludeOp(op);
      size--;
    }
    else
    {
      op->Idx = 1;
      op = op->Next;
    }
  }

  if (size < 3) size = 0;
  out_poly.resize(size);
  for (size_t i = 0; i < size; ++i)
  {
    out_poly[i] = op->Pt;
    op = op->Next;
  }
}
//------------------------------------------------------------------------------

void CleanPolygon(Path& poly, double distance)
{
  CleanPolygon(poly, poly, distance);
}
//------------------------------------------------------------------------------

void CleanPolygons(const Paths& in_polys, Paths& out_polys, double distance)
{
  out_polys.resize(in_polys.size());
  for (Paths::size_type i = 0; i < in_polys.size(); ++i)
    CleanPolygon(in_polys[i], out_polys[i], distance);
}
//------------------------------------------------------------------------------

void CleanPolygons(Paths& polys, double distance)
{
  CleanPolygons(polys, polys, distance);
}
//------------------------------------------------------------------------------

void Minkowski(const Path& poly, const Path& path, 
  Paths& solution, bool isSum, bool isClosed)
{
  int delta = (isClosed ? 1 : 0);
  size_t polyCnt = poly.size();
  size_t pathCnt = path.size();
  Paths pp;
  pp.reserve(pathCnt);
  if (isSum)
    for (size_t i = 0; i < pathCnt; ++i)
    {
      Path p;
      p.reserve(polyCnt);
      for (size_t j = 0; j < poly.size(); ++j)
        p.emplace_back(IntPoint2d(path[i].x() + poly[j].x(), path[i].y() + poly[j].y()));
      pp.emplace_back(p);
    }
  else
    for (size_t i = 0; i < pathCnt; ++i)
    {
      Path p;
      p.reserve(polyCnt);
      for (size_t j = 0; j < poly.size(); ++j)
        p.emplace_back(IntPoint2d(path[i].x() - poly[j].x(), path[i].y() - poly[j].y()));
      pp.emplace_back(p);
    }

  solution.clear();
  solution.reserve((pathCnt + delta) * (polyCnt + 1));
  for (size_t i = 0; i < pathCnt - 1 + delta; ++i)
    for (size_t j = 0; j < polyCnt; ++j)
    {
      Path quad;
      quad.reserve(4);
      quad.emplace_back(pp[i % pathCnt][j % polyCnt]);
      quad.emplace_back(pp[(i + 1) % pathCnt][j % polyCnt]);
      quad.emplace_back(pp[(i + 1) % pathCnt][(j + 1) % polyCnt]);
      quad.emplace_back(pp[i % pathCnt][(j + 1) % polyCnt]);
      if (!Orientation(quad)) ReversePath(quad);
      solution.emplace_back(quad);
    }
}
//------------------------------------------------------------------------------

void MinkowskiSum(const Path& pattern, const Path& path, Paths& solution, bool pathIsClosed)
{
  Minkowski(pattern, path, solution, true, pathIsClosed);
  Clipper c;
  c.AddPaths(solution, ptSubject, true);
  c.Execute(ctUnion, solution, pftNonZero, pftNonZero);
}
//------------------------------------------------------------------------------

void TranslatePath(const Path& input, Path& output, const IntPoint& delta)
{
  //precondition: input != output
  output.resize(input.size());
  for (size_t i = 0; i < input.size(); ++i)
    output[i] = IntPoint2d(input[i].x() + delta.x(), input[i].y() + delta.y());
}
//------------------------------------------------------------------------------

void MinkowskiSum(const Path& pattern, const Paths& paths, Paths& solution, bool pathIsClosed)
{
  Clipper c;
  for (size_t i = 0; i < paths.size(); ++i)
  {
    Paths tmp;
    Minkowski(pattern, paths[i], tmp, true, pathIsClosed);
    c.AddPaths(tmp, ptSubject, true);
    if (pathIsClosed)
    {
      Path tmp2;
      TranslatePath(paths[i], tmp2, pattern[0]);
      c.AddPath(tmp2, ptClip, true);
    }
  }
    c.Execute(ctUnion, solution, pftNonZero, pftNonZero);
}
//------------------------------------------------------------------------------

void MinkowskiDiff(const Path& poly1, const Path& poly2, Paths& solution)
{
  Minkowski(poly1, poly2, solution, false, true);
  Clipper c;
  c.AddPaths(solution, ptSubject, true);
  c.Execute(ctUnion, solution, pftNonZero, pftNonZero);
}
//------------------------------------------------------------------------------

enum NodeType {ntAny, ntOpen, ntClosed};

void AddPolyNodeToPaths(const PolyNode& polynode, NodeType nodetype, Paths& paths)
{
  bool match = true;
  if (nodetype == ntClosed) match = !polynode.IsOpen();
  else if (nodetype == ntOpen) return;

  if (!polynode.Contour.empty() && match)
    paths.emplace_back(polynode.Contour);
  for (int i = 0; i < polynode.ChildCount(); ++i)
    AddPolyNodeToPaths(*polynode.Childs[i], nodetype, paths);
}

void AddPolyNodeToPaths(PolyNode&& polynode, NodeType nodetype, Paths& paths)
{
  bool match = true;
  if (nodetype == ntClosed) match = !polynode.IsOpen();
  else if (nodetype == ntOpen) return;

  if (!polynode.Contour.empty() && match)
    paths.emplace_back(std::move(polynode.Contour));
  for (int i = 0; i < polynode.ChildCount(); ++i)
    AddPolyNodeToPaths(std::move(*polynode.Childs[i]), nodetype, paths);
}

//------------------------------------------------------------------------------

void PolyTreeToPaths(const PolyTree& polytree, Paths& paths)
{
  paths.clear();
  paths.reserve(polytree.Total());
  AddPolyNodeToPaths(polytree, ntAny, paths);
}

void PolyTreeToPaths(PolyTree&& polytree, Paths& paths)
{
  paths.clear();
  paths.reserve(polytree.Total());
  AddPolyNodeToPaths(std::move(polytree), ntAny, paths);
}

//------------------------------------------------------------------------------

void ClosedPathsFromPolyTree(const PolyTree& polytree, Paths& paths)
{
  paths.clear();
  paths.reserve(polytree.Total());
  AddPolyNodeToPaths(polytree, ntClosed, paths);
}
//------------------------------------------------------------------------------

void OpenPathsFromPolyTree(PolyTree& polytree, Paths& paths)
{
  paths.clear();
  paths.reserve(polytree.Total());
  //Open paths are top level only, so ...
  for (int i = 0; i < polytree.ChildCount(); ++i)
    if (polytree.Childs[i]->IsOpen())
      paths.emplace_back(polytree.Childs[i]->Contour);
}
//------------------------------------------------------------------------------

std::ostream& operator <<(std::ostream &s, const IntPoint &p)
{
  s << "(" << p.x() << "," << p.y() << ")";
  return s;
}
//------------------------------------------------------------------------------

std::ostream& operator <<(std::ostream &s, const Path &p)
{
  if (p.empty()) return s;
  Path::size_type last = p.size() -1;
  for (Path::size_type i = 0; i < last; i++)
    s << "(" << p[i].x() << "," << p[i].y() << "), ";
  s << "(" << p[last].x() << "," << p[last].y() << ")\n";
  return s;
}
//------------------------------------------------------------------------------

std::ostream& operator <<(std::ostream &s, const Paths &p)
{
  for (Paths::size_type i = 0; i < p.size(); i++)
    s << p[i];
  s << "\n";
  return s;
}
//------------------------------------------------------------------------------

} //ClipperLib namespace

#ifdef CLIPPERLIB_NAMESPACE_PREFIX
} // namespace CLIPPERLIB_NAMESPACE_PREFIX
#endif // CLIPPERLIB_NAMESPACE_PREFIX
