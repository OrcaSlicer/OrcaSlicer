//-----------------------------------------------------------------------------
// Functions relating to rational polynomial surfaces, which are trimmed by
// curves (either rational polynomial curves, or piecewise linear
// approximations to curves of intersection that can't be represented
// exactly in ratpoly form), and assembled into watertight shells.
//
// Copyright 2008-2013 Jonathan Westhues.
//-----------------------------------------------------------------------------

#ifndef __SURFACE_H
#define __SURFACE_H

// Utility functions, Bernstein polynomials of order 1-3 and their derivatives.
double Bernstein(int k, int deg, double t);
double BernsteinDerivative(int k, int deg, double t);

class SBezierList;
class SSurface;
class SCurvePt;

// Utility data structure, a two-dimensional BSP to accelerate polygon
// operations.
class SBspUv {
public:
    Point2d  a, b;

    SBspUv  *pos;
    SBspUv  *neg;

    SBspUv  *more;

    enum class Class : uint32_t {
        INSIDE            = 100,
        OUTSIDE           = 200,
        EDGE_PARALLEL     = 300,
        EDGE_ANTIPARALLEL = 400,
        EDGE_OTHER        = 500
    };

    static SBspUv *Alloc();
    static SBspUv *From(SEdgeList *el, SSurface *srf);

    void ScalePoints(Point2d *pt, Point2d *a, Point2d *b, SSurface *srf) const;
    double ScaledSignedDistanceToLine(Point2d pt, Point2d a, Point2d b,
        SSurface *srf) const;
    double ScaledDistanceToLine(Point2d pt, Point2d a, Point2d b, bool asSegment,
        SSurface *srf) const;

    void InsertEdge(Point2d a, Point2d b, SSurface *srf);
    static SBspUv *InsertOrCreateEdge(SBspUv *where, Point2d ea, Point2d eb, SSurface *srf);
    Class ClassifyPoint(Point2d p, Point2d eb, SSurface *srf) const;
    Class ClassifyEdge(Point2d ea, Point2d eb, SSurface *srf) const;
    double MinimumDistanceToEdge(Point2d p, SSurface *srf) const;
};

// Now the data structures to represent a shell of trimmed rational polynomial
// surfaces.

class SShell;

class hSSurface {
public:
    uint32_t v;
};

class hSCurve {
public:
    uint32_t v;
};

// Stuff for rational polynomial curves, of degree one to three. These are
// our inputs, and are also calculated for certain exact surface-surface
// intersections.
class SBezier {
public:
    int             tag;
    int             auxA, auxB;

    int             deg;
    Vector          ctrl[4];
    double          weight[4];
    uint32_t        entity;

    Vector PointAt(double t) const;
    Vector TangentAt(double t) const;
    void ClosestPointTo(Vector p, double *t, bool mustConverge=true) const;
    void SplitAt(double t, SBezier *bef, SBezier *aft) const;
    bool PointOnThisAndCurve(const SBezier *sbb, Vector *p) const;

    Vector Start() const;
    Vector Finish() const;
    bool Equals(SBezier *b) const;
    void MakePwlInto(SEdgeList *sel, double chordTol=0) const;
    void MakePwlInto(List<SCurvePt> *l, double chordTol=0) const;
    void MakePwlInto(SContour *sc, double chordTol=0) const;
    void MakePwlInto(List<Vector> *l, double chordTol=0) const;
    void MakePwlWorker(List<Vector> *l, double ta, double tb, double chordTol) const;
    void MakePwlInitialWorker(List<Vector> *l, double ta, double tb, double chordTol) const;
    void MakeNonrationalCubicInto(SBezierList *bl, double tolerance, int depth = 0) const;

    void AllIntersectionsWith(const SBezier *sbb, SPointList *spl) const;
    void GetBoundingProjd(Vector u, Vector orig, double *umin, double *umax) const;
    void Reverse();

    bool IsInPlane(Vector n, double d) const;
    bool IsCircle(Vector axis, Vector *center, double *r) const;
    bool IsRational() const;

    SBezier TransformedBy(Vector t, Quaternion q, double scale) const;
    SBezier InPerspective(Vector u, Vector v, Vector n,
                          Vector origin, double cameraTan) const;
    void ScaleSelfBy(double s);

    static SBezier From(Vector p0, Vector p1, Vector p2, Vector p3);
    static SBezier From(Vector p0, Vector p1, Vector p2);
    static SBezier From(Vector p0, Vector p1);
    static SBezier From(Vector4 p0, Vector4 p1, Vector4 p2, Vector4 p3);
    static SBezier From(Vector4 p0, Vector4 p1, Vector4 p2);
    static SBezier From(Vector4 p0, Vector4 p1);
};

class SBezierList {
public:
    List<SBezier>   l;

    void Clear();
    void ScaleSelfBy(double s);
    void CullIdenticalBeziers();
    void AllIntersectionsWith(SBezierList *sblb, SPointList *spl) const;
    bool GetPlaneContainingBeziers(Vector *p, Vector *u, Vector *v,
                                        Vector *notCoplanarAt) const;
};

class SBezierLoop {
public:
    int             tag;
    List<SBezier>   l;

    inline void Clear() { l.Clear(); }
    bool IsClosed() const;
    void Reverse();
    void MakePwlInto(SContour *sc, double chordTol=0) const;
    void GetBoundingProjd(Vector u, Vector orig, double *umin, double *umax) const;

    static SBezierLoop FromCurves(SBezierList *spcl,
                                  bool *allClosed, SEdge *errorAt);
};

class SBezierLoopSet {
public:
    List<SBezierLoop> l;
    Vector normal;
    Vector point;
    double area;

    static SBezierLoopSet From(SBezierList *spcl, SPolygon *poly,
                               double chordTol,
                               bool *allClosed, SEdge *errorAt,
                               SBezierList *openContours);

    void GetBoundingProjd(Vector u, Vector orig, double *umin, double *umax) const;
    double SignedArea();
    void MakePwlInto(SPolygon *sp) const;
    void Clear();
};

class SBezierLoopSetSet {
public:
    List<SBezierLoopSet>    l;

    void FindOuterFacesFrom(SBezierList *sbl, SPolygon *spxyz, SSurface *srfuv,
                            double chordTol,
                            bool *allClosed, SEdge *notClosedAt,
                            bool *allCoplanar, Vector *notCoplanarAt,
                            SBezierList *openContours);
    void AddOpenPath(SBezier *sb);
    void Clear();
};

// Stuff for the surface trim curves: piecewise linear
class SCurvePt {
public:
    int         tag;
    Vector      p;
    bool        vertex;
};

class SCurve {
public:
    hSCurve         h;

    // In a Boolean, C = A op B. The curves in A and B get copied into C, and
    // therefore must get new hSCurves assigned. For the curves in A and B,
    // we use newH to record their new handle in C.
    hSCurve         newH;
    enum class Source : uint32_t {
        A             = 100,
        B             = 200,
        INTERSECTION  = 300
    };
    Source          source;

    bool            isExact;
    SBezier         exact;

    List<SCurvePt>  pts;

    hSSurface       surfA;
    hSSurface       surfB;

    static SCurve FromTransformationOf(SCurve *a, Vector t,
                                       Quaternion q, double scale);
    SCurve MakeCopySplitAgainst(SShell *agnstA, SShell *agnstB,
                                SSurface *srfA, SSurface *srfB) const;
    void RemoveShortSegments(SSurface *srfA, SSurface *srfB);
    SSurface *GetSurfaceA(SShell *a, SShell *b) const;
    SSurface *GetSurfaceB(SShell *a, SShell *b) const;

    void Clear();
};

// A segment of a curve by which a surface is trimmed: indicates which curve,
// by its handle, and the starting and ending points of our segment of it.
// The vector out points out of the surface; it, the surface outer normal,
// and a tangent to the beginning of the curve are all orthogonal.
class STrimBy {
public:
    hSCurve     curve;
    bool        backwards;
    // If a trim runs backwards, then start and finish still correspond to
    // the actual start and finish, but they appear in reverse order in
    // the referenced curve.
    Vector      start;
    Vector      finish;

    static STrimBy EntireCurve(SShell *shell, hSCurve hsc, bool backwards);
};

// An intersection point between a line and a surface
class SInter {
public:
    int         tag;
    Vector      p;
    SSurface    *srf;
    Point2d     pinter;
    Vector      surfNormal;     // of the intersecting surface, at pinter
    bool        onEdge;         // pinter is on edge of trim poly
};

// A rational polynomial surface in Bezier form.
class SSurface {
public:

    enum class CombineAs : uint32_t {
        UNION      = 10,
        DIFFERENCE = 11,
        INTERSECT  = 12
    };

    int             tag;
    hSSurface       h;

    // Same as newH for the curves; record what a surface gets renamed to
    // when I copy things over.
    hSSurface       newH;

    RgbaColor       color;
    uint32_t        face;

    int             degm, degn;
    Vector          ctrl[4][4];
    double          weight[4][4];

    List<STrimBy>   trim;

    // For testing whether a point (u, v) on the surface lies inside the trim
    SBspUv          *bsp;
    SEdgeList       edges;

    // For caching our initial (u, v) when doing Newton iterations to project
    // a point into our surface.
    Point2d         cached;

    static SSurface FromExtrusionOf(SBezier *spc, Vector t0, Vector t1);
    static SSurface FromRevolutionOf(SBezier *sb, Vector pt, Vector axis,
                                        double thetas, double thetaf);
    static SSurface FromPlane(Vector pt, Vector u, Vector v);
    static SSurface FromTransformationOf(SSurface *a, Vector t, Quaternion q,
                                         double scale,
                                         bool includingTrims);
    void ScaleSelfBy(double s);

    void EdgeNormalsWithinSurface(Point2d auv, Point2d buv,
                                  Vector *pt, Vector *enin, Vector *enout,
                                  Vector *surfn,
                                  uint32_t auxA,
                                  SShell *shell, SShell *sha, SShell *shb);
    void FindChainAvoiding(SEdgeList *src, SEdgeList *dest, SPointList *avoid);
    SSurface MakeCopyTrimAgainst(SShell *parent, SShell *a, SShell *b,
                                    SShell *into, SSurface::CombineAs type);
    void TrimFromEdgeList(SEdgeList *el, bool asUv);
    void IntersectAgainst(SSurface *b, SShell *agnstA, SShell *agnstB,
                          SShell *into);
    void AddExactIntersectionCurve(SBezier *sb, SSurface *srfB,
                          SShell *agnstA, SShell *agnstB, SShell *into);

    typedef struct {
        int     tag;
        Point2d p;
    } Inter;
    void WeightControlPoints();
    void UnWeightControlPoints();
    void CopyRowOrCol(bool row, int this_ij, SSurface *src, int src_ij);
    void BlendRowOrCol(bool row, int this_ij, SSurface *a, int a_ij,
                                              SSurface *b, int b_ij);
    double DepartureFromCoplanar() const;
    void SplitInHalf(bool byU, SSurface *sa, SSurface *sb);
    void AllPointsIntersecting(Vector a, Vector b,
                               List<SInter> *l,
                               bool asSegment, bool trimmed, bool inclTangent);
    void AllPointsIntersectingUntrimmed(Vector a, Vector b,
                                        int *cnt, int *level,
                                        List<Inter> *l, bool asSegment,
                                        SSurface *sorig);

    void ClosestPointTo(Vector p, Point2d *puv, bool mustConverge=true);
    void ClosestPointTo(Vector p, double *u, double *v, bool mustConverge=true);
    bool ClosestPointNewton(Vector p, double *u, double *v, bool mustConverge=true) const;

    bool PointIntersectingLine(Vector p0, Vector p1, double *u, double *v) const;
    Vector ClosestPointOnThisAndSurface(SSurface *srf2, Vector p);
    void PointOnSurfaces(SSurface *s1, SSurface *s2, double *u, double *v);
    Vector PointAt(double u, double v) const;
    Vector PointAt(Point2d puv) const;
    void TangentsAt(double u, double v, Vector *tu, Vector *tv) const;
    Vector NormalAt(Point2d puv) const;
    Vector NormalAt(double u, double v) const;
    bool LineEntirelyOutsideBbox(Vector a, Vector b, bool asSegment) const;
    void GetAxisAlignedBounding(Vector *ptMax, Vector *ptMin) const;
    bool CoincidentWithPlane(Vector n, double d) const;
    bool CoincidentWith(SSurface *ss, bool sameNormal) const;
    bool IsExtrusion(SBezier *of, Vector *along) const;
    bool IsCylinder(Vector *axis, Vector *center, double *r,
                        Vector *start, Vector *finish) const;

    void TriangulateInto(SShell *shell, SMesh *sm);

    // these are intended as bitmasks, even though there's just one now
    enum class MakeAs : uint32_t {
       UV  = 0x01,
       XYZ = 0x00
    };
    void MakeTrimEdgesInto(SEdgeList *sel, MakeAs flags, SCurve *sc, STrimBy *stb);
    void MakeEdgesInto(SShell *shell, SEdgeList *sel, MakeAs flags,
                       SShell *useCurvesFrom=NULL);

    Vector ExactSurfaceTangentAt(Vector p, SSurface *srfA, SSurface *srfB,
                                Vector dir);
    void MakeSectionEdgesInto(SShell *shell, SEdgeList *sel, SBezierList *sbl);
    void MakeClassifyingBsp(SShell *shell, SShell *useCurvesFrom);
    double ChordToleranceForEdge(Vector a, Vector b) const;
    void MakeTriangulationGridInto(List<double> *l, double vs, double vf,
                                    bool swapped) const;
    Vector PointAtMaybeSwapped(double u, double v, bool swapped) const;

    void Reverse();
    void Clear();
};

class SShell {
public:
    IdList<SCurve,hSCurve>      curve;
    IdList<SSurface,hSSurface>  surface;

    bool                        booleanFailed;

    void MakeFromExtrusionOf(SBezierLoopSet *sbls, Vector t0, Vector t1,
                             RgbaColor color);
    void MakeFromRevolutionOf(SBezierLoopSet *sbls, Vector pt, Vector axis,
                              RgbaColor color, Group *group);

    void MakeFromUnionOf(SShell *a, SShell *b);
    void MakeFromDifferenceOf(SShell *a, SShell *b);
    void MakeFromBoolean(SShell *a, SShell *b, SSurface::CombineAs type);
    void CopyCurvesSplitAgainst(bool opA, SShell *agnst, SShell *into);
    void CopySurfacesTrimAgainst(SShell *sha, SShell *shb, SShell *into, SSurface::CombineAs type);
    void MakeIntersectionCurvesAgainst(SShell *against, SShell *into);
    void MakeClassifyingBsps(SShell *useCurvesFrom);
    void AllPointsIntersecting(Vector a, Vector b, List<SInter> *il,
                                bool asSegment, bool trimmed, bool inclTangent);
    void MakeCoincidentEdgesInto(SSurface *proto, bool sameNormal,
                                 SEdgeList *el, SShell *useCurvesFrom);
    void RewriteSurfaceHandlesForCurves(SShell *a, SShell *b);
    void CleanupAfterBoolean();

    // Definitions when classifying regions of a surface; it is either inside,
    // outside, or coincident (with parallel or antiparallel normal) with a
    // shell.
    enum class Class : uint32_t {
        INSIDE     = 100,
        OUTSIDE    = 200,
        COINC_SAME = 300,
        COINC_OPP  = 400
    };
    static const double DOTP_TOL;
    Class ClassifyRegion(Vector edge_n, Vector inter_surf_n,
                       Vector edge_surf_n) const;

    bool ClassifyEdge(Class *indir, Class *outdir,
                      Vector ea, Vector eb,
                      Vector p, Vector edge_n_in,
                      Vector edge_n_out, Vector surf_n);

    void MakeFromCopyOf(SShell *a);
    void MakeFromTransformationOf(SShell *a,
                                  Vector trans, Quaternion q, double scale);
    void MakeFromAssemblyOf(SShell *a, SShell *b);
    void MergeCoincidentSurfaces();

    void TriangulateInto(SMesh *sm);
    void MakeEdgesInto(SEdgeList *sel);
    void MakeSectionEdgesInto(Vector n, double d, SEdgeList *sel, SBezierList *sbl);
    bool IsEmpty() const;
    void RemapFaces(Group *g, int remap);
    void Clear();
};

#endif

