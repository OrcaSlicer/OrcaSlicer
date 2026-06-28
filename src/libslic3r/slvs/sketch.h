//-----------------------------------------------------------------------------
// The parametric structure of our sketch, in multiple groups, that generate
// geometric entities and surfaces.
//
// Copyright 2008-2013 Jonathan Westhues.
//-----------------------------------------------------------------------------

#ifndef __SKETCH_H
#define __SKETCH_H

class hGroup;
class hRequest;
class hEntity;
class hParam;
class hStyle;
class hConstraint;
class hEquation;

class Entity;
class Param;
class Equation;
class Style;

enum class PolyError : uint32_t {
    GOOD              = 0,
    NOT_CLOSED        = 1,
    NOT_COPLANAR      = 2,
    SELF_INTERSECTING = 3,
    ZERO_LEN_EDGE     = 4
};

enum class StipplePattern : uint32_t {
    CONTINUOUS     = 0,
    SHORT_DASH     = 1,
    DASH           = 2,
    LONG_DASH      = 3,
    DASH_DOT       = 4,
    DASH_DOT_DOT   = 5,
    DOT            = 6,
    FREEHAND       = 7,
    ZIGZAG         = 8,

    LAST           = ZIGZAG
};

const std::vector<double> &StipplePatternDashes(StipplePattern pattern);
double StipplePatternLength(StipplePattern pattern);

enum class Command : uint32_t;

// All of the hWhatever handles are a 32-bit ID, that is used to represent
// some data structure in the sketch.
class hGroup {
public:
    // bits 15: 0   -- group index
    uint32_t v;

    inline hEntity entity(int i) const;
    inline hParam param(int i) const;
    inline hEquation equation(int i) const;
};
class hRequest {
public:
    // bits 15: 0   -- request index
    uint32_t v;

    inline hEntity entity(int i) const;
    inline hParam param(int i) const;

    inline bool IsFromReferences() const;
};
class hEntity {
public:
    // bits 15: 0   -- entity index
    //      31:16   -- request index
    uint32_t v;

    inline bool isFromRequest() const;
    inline hRequest request() const;
    inline hGroup group() const;
    inline hEquation equation(int i) const;
};
class hParam {
public:
    // bits 15: 0   -- param index
    //      31:16   -- request index
    uint32_t v;

    inline hRequest request() const;
};

class hStyle {
public:
    uint32_t v;
};

class EntityId {
public:
    uint32_t v;     // entity ID, starting from 0
};
class EntityMap {
public:
    int         tag;

    EntityId    h;
    hEntity     input;
    int         copyNumber;
    // (input, copyNumber) gets mapped to ((Request)xxx).entity(h.v)

    void Clear() {}
};

// A set of requests. Every request must have an associated group.
class Group {
public:
    static const hGroup     HGROUP_REFERENCES;

    int         tag;
    hGroup      h;

    enum class CopyAs {
        NUMERIC,
        N_TRANS,
        N_ROT_AA,
        N_ROT_TRANS,
    };

    enum class Type : uint32_t {
        DRAWING_3D                    = 5000,
        DRAWING_WORKPLANE             = 5001,
        EXTRUDE                       = 5100,
        LATHE                         = 5101,
        ROTATE                        = 5200,
        TRANSLATE                     = 5201,
        LINKED                        = 5300
    };
    Group::Type type;

    int order;

    hGroup      opA;
    hGroup      opB;
    bool        visible;
    bool        suppress;
    bool        relaxConstraints;
    bool        allowRedundant;
    bool        allDimsReference;
    double      scale;

    bool        clean;
    bool        dofCheckOk;
    hEntity     activeWorkplane;
    double      valA;
    double      valB;
    double      valC;
    RgbaColor   color;

    struct {
        SolveResult         how;
        int                 dof;
        List<hConstraint>   remove;
    } solved;

    enum class Subtype : uint32_t {
        // For drawings in 2d
        WORKPLANE_BY_POINT_ORTHO   = 6000,
        WORKPLANE_BY_LINE_SEGMENTS = 6001,
        // For extrudes, translates, and rotates
        ONE_SIDED                  = 7000,
        TWO_SIDED                  = 7001
    };
    Group::Subtype subtype;

    bool skipFirst; // for step and repeat ops

    struct {
        Quaternion  q;
        hEntity     origin;
        hEntity     entityB;
        hEntity     entityC;
        bool        swapUV;
        bool        negateU;
        bool        negateV;
    } predef;

    SPolygon                polyLoops;
    SBezierLoopSetSet       bezierLoops;
    SBezierList             bezierOpens;

    struct {
        PolyError       how;
        SEdge           notClosedAt;
        Vector          errorPointAt;
    }               polyError;

    bool            booleanFailed;

    SShell          thisShell;
    SShell          runningShell;

    SMesh           thisMesh;
    SMesh           runningMesh;

    bool            displayDirty;
    SMesh           displayMesh;
    SOutlineList    displayOutlines;

    enum class CombineAs : uint32_t {
        UNION           = 0,
        DIFFERENCE      = 1,
        ASSEMBLE        = 2
    };
    CombineAs meshCombine;

    bool forceToMesh;

    IdList<EntityMap,EntityId> remap;
    enum { REMAP_PRIME = 19477 };
    int remapCache[REMAP_PRIME];

    Platform::Path linkFile;
    SMesh       impMesh;
    SShell      impShell;
    EntityList  impEntity;

    std::string     name;


    void Activate();
    std::string DescriptionString();
    void Clear();

    static void AddParam(ParamList *param, hParam hp, double v);
    void Generate(EntityList *entity, ParamList *param);
    bool IsSolvedOkay();
    void TransformImportedBy(Vector t, Quaternion q);
    bool IsForcedToMeshBySource() const;
    bool IsForcedToMesh() const;
    // When a request generates entities from entities, and the source
    // entities may have come from multiple requests, it's necessary to
    // remap the entity ID so that it's still unique. We do this with a
    // mapping list.
    enum {
        REMAP_LAST         = 1000,
        REMAP_TOP          = 1001,
        REMAP_BOTTOM       = 1002,
        REMAP_PT_TO_LINE   = 1003,
        REMAP_LINE_TO_FACE = 1004,
        REMAP_LATHE_START  = 1006,
        REMAP_LATHE_END    = 1007,
        REMAP_PT_TO_ARC    = 1008,
        REMAP_PT_TO_NORMAL = 1009,
    };
    hEntity Remap(hEntity in, int copyNumber);
    void MakeExtrusionLines(EntityList *el, hEntity in);
    void MakeLatheCircles(IdList<Entity,hEntity> *el, IdList<Param,hParam> *param, hEntity in, Vector pt, Vector axis, int ai);
    void MakeExtrusionTopBottomFaces(EntityList *el, hEntity pt);
    void CopyEntity(EntityList *el,
                    Entity *ep, int timesApplied, int remap,
                    hParam dx, hParam dy, hParam dz,
                    hParam qw, hParam qvx, hParam qvy, hParam qvz,
                    CopyAs as);

    void AddEq(IdList<Equation,hEquation> *l, Expr *expr, int index);
    void GenerateEquations(IdList<Equation,hEquation> *l);
    bool IsVisible();
    int GetNumConstraints();
    Vector ExtrusionGetVector();
    void ExtrusionForceVectorTo(const Vector &v);

    // Assembling the curves into loops, and into a piecewise linear polygon
    // at the same time.
    void AssembleLoops(bool *allClosed, bool *allCoplanar, bool *allNonZeroLen);
    void GenerateLoops();
    // And the mesh stuff
    Group *PreviousGroup() const;
    Group *RunningMeshGroup() const;
    bool IsMeshGroup();

    void GenerateShellAndMesh();
    template<class T> void GenerateForStepAndRepeat(T *steps, T *outs, Group::CombineAs forWhat);
    template<class T> void GenerateForBoolean(T *a, T *b, T *o, Group::CombineAs how);
    void GenerateDisplayItems();

    enum class DrawMeshAs { DEFAULT, HOVERED, SELECTED };
    void DrawMesh(DrawMeshAs how, Canvas *canvas);
    void Draw(Canvas *canvas);
    void DrawPolyError(Canvas *canvas);
    void DrawFilledPaths(Canvas *canvas);
    void DrawContourAreaLabels(Canvas *canvas);

    SPolygon GetPolygon();

    static void MenuGroup(Command id);
};

// A user request for some primitive or derived operation; for example a
// line, or a step and repeat.
class Request {
public:
    // Some predefined requests, that are present in every sketch.
    static const hRequest   HREQUEST_REFERENCE_XY;
    static const hRequest   HREQUEST_REFERENCE_YZ;
    static const hRequest   HREQUEST_REFERENCE_ZX;

    int         tag;
    hRequest    h;

    // Types of requests
    enum class Type : uint32_t {
        WORKPLANE              = 100,
        DATUM_POINT            = 101,
        LINE_SEGMENT           = 200,
        CUBIC                  = 300,
        CUBIC_PERIODIC         = 301,
        CIRCLE                 = 400,
        ARC_OF_CIRCLE          = 500,
        TTF_TEXT               = 600,
        IMAGE                  = 700
    };

    Request::Type type;
    int         extraPoints;

    hEntity     workplane; // or Entity::FREE_IN_3D
    hGroup      group;
    hStyle      style;

    bool        construction;

    std::string str;
    std::string font;
    Platform::Path file;
    double      aspectRatio;

    static hParam AddParam(ParamList *param, hParam hp);
    void Generate(EntityList *entity, ParamList *param);

    std::string DescriptionString() const;
    int IndexOfPoint(hEntity he) const;

    void Clear() {}
};

#define MAX_POINTS_IN_ENTITY (12)
class EntityBase {
public:
    int         tag;
    hEntity     h;

    static const hEntity    FREE_IN_3D;
    static const hEntity    NO_ENTITY;

    enum class Type : uint32_t {
        POINT_IN_3D            =  2000,
        POINT_IN_2D            =  2001,
        POINT_N_TRANS          =  2010,
        POINT_N_ROT_TRANS      =  2011,
        POINT_N_COPY           =  2012,
        POINT_N_ROT_AA         =  2013,

        NORMAL_IN_3D           =  3000,
        NORMAL_IN_2D           =  3001,
        NORMAL_N_COPY          =  3010,
        NORMAL_N_ROT           =  3011,
        NORMAL_N_ROT_AA        =  3012,

        DISTANCE               =  4000,
        DISTANCE_N_COPY        =  4001,

        FACE_NORMAL_PT         =  5000,
        FACE_XPROD             =  5001,
        FACE_N_ROT_TRANS       =  5002,
        FACE_N_TRANS           =  5003,
        FACE_N_ROT_AA          =  5004,

        WORKPLANE              = 10000,
        LINE_SEGMENT           = 11000,
        CUBIC                  = 12000,
        CUBIC_PERIODIC         = 12001,
        CIRCLE                 = 13000,
        ARC_OF_CIRCLE          = 14000,
        TTF_TEXT               = 15000,
        IMAGE                  = 16000
    };

    Type        type;

    hGroup      group;
    hEntity     workplane;   // or Entity::FREE_IN_3D

    // When it comes time to draw an entity, we look here to get the
    // defining variables.
    hEntity     point[MAX_POINTS_IN_ENTITY];
    int         extraPoints;
    hEntity     normal;
    hEntity     distance;
    // The only types that have their own params are points, normals,
    // and directions.
    hParam      param[7];

    // Transformed points/normals/distances have their numerical base
    Vector      numPoint;
    Quaternion  numNormal;
    double      numDistance;

    std::string str;
    std::string font;
    Platform::Path file;
    double      aspectRatio;

    // For entities that are derived by a transformation, the number of
    // times to apply the transformation.
    int timesApplied;

    Quaternion GetAxisAngleQuaternion(int param0) const;
    ExprQuaternion GetAxisAngleQuaternionExprs(int param0) const;

    bool IsCircle() const;
    Expr *CircleGetRadiusExpr() const;
    double CircleGetRadiusNum() const;
    void ArcGetAngles(double *thetaa, double *thetab, double *dtheta) const;

    bool HasVector() const;
    ExprVector VectorGetExprs() const;
    ExprVector VectorGetExprsInWorkplane(hEntity wrkpl) const;
    Vector VectorGetNum() const;
    Vector VectorGetRefPoint() const;
    Vector VectorGetStartPoint() const;

    // For distances
    bool IsDistance() const;
    double DistanceGetNum() const;
    Expr *DistanceGetExpr() const;
    void DistanceForceTo(double v);

    bool IsWorkplane() const;
    // The plane is points P such that P dot (xn, yn, zn) - d = 0
    void WorkplaneGetPlaneExprs(ExprVector *n, Expr **d) const;
    ExprVector WorkplaneGetOffsetExprs() const;
    Vector WorkplaneGetOffset() const;
    EntityBase *Normal() const;

    bool IsFace() const;
    ExprVector FaceGetNormalExprs() const;
    Vector FaceGetNormalNum() const;
    ExprVector FaceGetPointExprs() const;
    Vector FaceGetPointNum() const;

    bool IsPoint() const;
    // Applies for any of the point types
    Vector PointGetNum() const;
    ExprVector PointGetExprs() const;
    void PointGetExprsInWorkplane(hEntity wrkpl, Expr **u, Expr **v) const;
    ExprVector PointGetExprsInWorkplane(hEntity wrkpl) const;
    void PointForceTo(Vector v);
    void PointForceParamTo(Vector v);
    // These apply only the POINT_N_ROT_TRANS, which has an assoc rotation
    Quaternion PointGetQuaternion() const;
    void PointForceQuaternionTo(Quaternion q);

    bool IsNormal() const;
    // Applies for any of the normal types
    Quaternion NormalGetNum() const;
    ExprQuaternion NormalGetExprs() const;
    void NormalForceTo(Quaternion q);

    Vector NormalU() const;
    Vector NormalV() const;
    Vector NormalN() const;
    ExprVector NormalExprsU() const;
    ExprVector NormalExprsV() const;
    ExprVector NormalExprsN() const;

    Vector CubicGetStartNum() const;
    Vector CubicGetFinishNum() const;
    ExprVector CubicGetStartTangentExprs() const;
    ExprVector CubicGetFinishTangentExprs() const;
    Vector CubicGetStartTangentNum() const;
    Vector CubicGetFinishTangentNum() const;

    bool HasEndpoints() const;
    Vector EndpointStart() const;
    Vector EndpointFinish() const;

    void RectGetPointsExprs(ExprVector *eap, ExprVector *ebp) const;

    void AddEq(IdList<Equation,hEquation> *l, Expr *expr, int index) const;
    void GenerateEquations(IdList<Equation,hEquation> *l) const;

    void Clear() {}
};

class Entity : public EntityBase {
public:
    // Necessary for Entity e = {} to zero-initialize, since
    // classes with base classes are not aggregates and
    // the default constructor does not initialize members.
    //
    // Note EntityBase({}); without explicitly value-initializing
    // the base class, MSVC2013 will default-initialize it, leaving
    // POD members with indeterminate value.
    Entity() : EntityBase({}), forceHidden(), actPoint(), actNormal(),
        actDistance(), actVisible(), style(), construction(),
        beziers(), edges(), edgesChordTol(), screenBBox(), screenBBoxValid() {};

    // A linked entity that was hidden in the source file ends up hidden
    // here too.
    bool        forceHidden;

    // All points/normals/distances have their numerical value; this is
    // a convenience, to simplify the link/assembly code, so that the
    // part is entirely described by the entities.
    Vector      actPoint;
    Quaternion  actNormal;
    double      actDistance;
    // and the shown state also gets saved here, for later import
    bool        actVisible;

    hStyle      style;
    bool        construction;

    SBezierList beziers;
    SEdgeList   edges;
    double      edgesChordTol;
    BBox        screenBBox;
    bool        screenBBoxValid;

    bool IsStylable() const;
    bool IsVisible() const;

    enum class DrawAs { DEFAULT, OVERLAY, HIDDEN, HOVERED, SELECTED };
    void Draw(DrawAs how, Canvas *canvas);
    void GetReferencePoints(std::vector<Vector> *refs);
    int GetPositionOfPoint(const Camera &camera, Point2d p);

    void ComputeInterpolatingSpline(SBezierList *sbl, bool periodic) const;
    void GenerateBezierCurves(SBezierList *sbl) const;
    void GenerateEdges(SEdgeList *el);

    SBezierList *GetOrGenerateBezierCurves();
    SEdgeList *GetOrGenerateEdges();
    BBox GetOrGenerateScreenBBox(bool *hasBBox);

    void CalculateNumerical(bool forExport);

    std::string DescriptionString() const;

    void Clear() {
        beziers.l.Clear();
        edges.l.Clear();
    }
};

class EntReqTable {
public:
    static bool GetRequestInfo(Request::Type req, int extraPoints,
                               EntityBase::Type *ent, int *pts, bool *hasNormal, bool *hasDistance);
    static bool GetEntityInfo(EntityBase::Type ent, int extraPoints,
                              Request::Type *req, int *pts, bool *hasNormal, bool *hasDistance);
    static Request::Type GetRequestForEntity(EntityBase::Type ent);
};

class Param {
public:
    int         tag;
    hParam      h;

    double      val;
    bool        known;
    bool        free;

    // Used only in the solver
    hParam      substd;

    static const hParam NO_PARAM;

    void Clear() {}
};


class hConstraint {
public:
    uint32_t v;

    inline hEquation equation(int i) const;
    inline hParam param(int i) const;
};

class ConstraintBase {
public:
    int         tag;
    hConstraint h;

    static const hConstraint NO_CONSTRAINT;

    enum class Type : uint32_t {
        POINTS_COINCIDENT      =  20,
        PT_PT_DISTANCE         =  30,
        PT_PLANE_DISTANCE      =  31,
        PT_LINE_DISTANCE       =  32,
        PT_FACE_DISTANCE       =  33,
        PROJ_PT_DISTANCE       =  34,
        PT_IN_PLANE            =  41,
        PT_ON_LINE             =  42,
        PT_ON_FACE             =  43,
        EQUAL_LENGTH_LINES     =  50,
        LENGTH_RATIO           =  51,
        EQ_LEN_PT_LINE_D       =  52,
        EQ_PT_LN_DISTANCES     =  53,
        EQUAL_ANGLE            =  54,
        EQUAL_LINE_ARC_LEN     =  55,
        LENGTH_DIFFERENCE      =  56,
        SYMMETRIC              =  60,
        SYMMETRIC_HORIZ        =  61,
        SYMMETRIC_VERT         =  62,
        SYMMETRIC_LINE         =  63,
        AT_MIDPOINT            =  70,
        HORIZONTAL             =  80,
        VERTICAL               =  81,
        DIAMETER               =  90,
        PT_ON_CIRCLE           = 100,
        SAME_ORIENTATION       = 110,
        ANGLE                  = 120,
        PARALLEL               = 121,
        PERPENDICULAR          = 122,
        ARC_LINE_TANGENT       = 123,
        CUBIC_LINE_TANGENT     = 124,
        CURVE_CURVE_TANGENT    = 125,
        EQUAL_RADIUS           = 130,
        WHERE_DRAGGED          = 200,

        COMMENT                = 1000
    };

    Type        type;

    hGroup      group;
    hEntity     workplane;

    // These are the parameters for the constraint.
    double      valA;
    hParam      valP;
    hEntity     ptA;
    hEntity     ptB;
    hEntity     entityA;
    hEntity     entityB;
    hEntity     entityC;
    hEntity     entityD;
    bool        other;
    bool        other2;

    bool        reference;  // a ref dimension, that generates no eqs
    std::string comment;    // since comments are represented as constraints

    bool HasLabel() const;

    void Generate(IdList<Param, hParam> *param);

    void GenerateEquations(IdList<Equation,hEquation> *entity,
                           bool forReference = false) const;
    // Some helpers when generating symbolic constraint equations
    void ModifyToSatisfy();
    void AddEq(IdList<Equation,hEquation> *l, Expr *expr, int index) const;
    void AddEq(IdList<Equation,hEquation> *l, const ExprVector &v, int baseIndex = 0) const;
    static Expr *DirectionCosine(hEntity wrkpl, ExprVector ae, ExprVector be);
    static Expr *Distance(hEntity workplane, hEntity pa, hEntity pb);
    static Expr *PointLineDistance(hEntity workplane, hEntity pt, hEntity ln);
    static Expr *PointPlaneDistance(ExprVector p, hEntity plane);
    static ExprVector VectorsParallel3d(ExprVector a, ExprVector b, hParam p);
    static ExprVector PointInThreeSpace(hEntity workplane, Expr *u, Expr *v);

    void Clear() {}
};

class Constraint : public ConstraintBase {
public:
    // See Entity::Entity().
    Constraint() : ConstraintBase({}), disp() {}

    // These define how the constraint is drawn on-screen.
    struct {
        Vector      offset;
        hStyle      style;
    } disp;

    bool IsVisible() const;
    bool IsStylable() const;
    hStyle GetStyle() const;
    bool HasLabel() const;
    std::string Label() const;

    enum class DrawAs { DEFAULT, HOVERED, SELECTED };
    void Draw(DrawAs how, Canvas *canvas);
    Vector GetLabelPos(const Camera &camera);
    void GetReferencePoints(const Camera &camera, std::vector<Vector> *refs);

    void DoLayout(DrawAs how, Canvas *canvas,
                  Vector *labelPos, std::vector<Vector> *refs);
    void DoLine(Canvas *canvas, Canvas::hStroke hcs, Vector a, Vector b);
    void DoStippledLine(Canvas *canvas, Canvas::hStroke hcs, Vector a, Vector b);
    bool DoLineExtend(Canvas *canvas, Canvas::hStroke hcs,
                      Vector p0, Vector p1, Vector pt, double salient);
    void DoArcForAngle(Canvas *canvas, Canvas::hStroke hcs,
                       Vector a0, Vector da, Vector b0, Vector db,
                       Vector offset, Vector *ref, bool trim);
    void DoArrow(Canvas *canvas, Canvas::hStroke hcs,
                 Vector p, Vector dir, Vector n, double width, double angle, double da);
    void DoLineWithArrows(Canvas *canvas, Canvas::hStroke hcs,
                          Vector ref, Vector a, Vector b, bool onlyOneExt);
    int  DoLineTrimmedAgainstBox(Canvas *canvas, Canvas::hStroke hcs,
                                 Vector ref, Vector a, Vector b, bool extend,
                                 Vector gr, Vector gu, double swidth, double sheight);
    int  DoLineTrimmedAgainstBox(Canvas *canvas, Canvas::hStroke hcs,
                                 Vector ref, Vector a, Vector b, bool extend = true);
    void DoLabel(Canvas *canvas, Canvas::hStroke hcs,
                 Vector ref, Vector *labelPos, Vector gr, Vector gu);
    void DoProjectedPoint(Canvas *canvas, Canvas::hStroke hcs, Vector *p);
    void DoProjectedPoint(Canvas *canvas, Canvas::hStroke hcs, Vector *p, Vector n, Vector o);

    void DoEqualLenTicks(Canvas *canvas, Canvas::hStroke hcs,
                         Vector a, Vector b, Vector gn, Vector *refp);
    void DoEqualRadiusTicks(Canvas *canvas, Canvas::hStroke hcs,
                            hEntity he, Vector *refp);

    std::string DescriptionString() const;

    static hConstraint AddConstraint(Constraint *c, bool rememberForUndo);
    static hConstraint AddConstraint(Constraint *c);
    static void MenuConstrain(Command id);
    static void DeleteAllConstraintsFor(Constraint::Type type, hEntity entityA, hEntity ptA);

    static hConstraint ConstrainCoincident(hEntity ptA, hEntity ptB);
    static hConstraint Constrain(Constraint::Type type, hEntity ptA, hEntity ptB, hEntity entityA);
    static hConstraint Constrain(Constraint::Type type, hEntity ptA, hEntity ptB,
                                    hEntity entityA, hEntity entityB,
                                    bool other, bool other2);
};

class hEquation {
public:
    uint32_t v;

    inline bool isFromConstraint() const;
    inline hConstraint constraint() const;
};

class Equation {
public:
    int         tag;
    hEquation   h;

    Expr        *e;

    void Clear() {}
};


class Style {
public:
    int         tag;
    hStyle      h;

    enum {
        // If an entity has no style, then it will be colored according to
        // whether the group that it's in is active or not, whether it's
        // construction or not, and so on.
        NO_STYLE       = 0,

        ACTIVE_GRP     = 1,
        CONSTRUCTION   = 2,
        INACTIVE_GRP   = 3,
        DATUM          = 4,
        SOLID_EDGE     = 5,
        CONSTRAINT     = 6,
        SELECTED       = 7,
        HOVERED        = 8,
        CONTOUR_FILL   = 9,
        NORMALS        = 10,
        ANALYZE        = 11,
        DRAW_ERROR     = 12,
        DIM_SOLID      = 13,
        HIDDEN_EDGE    = 14,
        OUTLINE        = 15,

        FIRST_CUSTOM   = 0x100
    };

    std::string name;

    enum class UnitsAs : uint32_t {
        PIXELS   = 0,
        MM       = 1
    };
    double      width;
    UnitsAs     widthAs;
    double      textHeight;
    UnitsAs     textHeightAs;
    enum class TextOrigin : uint32_t {
        NONE    = 0x00,
        LEFT    = 0x01,
        RIGHT   = 0x02,
        BOT     = 0x04,
        TOP     = 0x08
    };
    TextOrigin  textOrigin;
    double      textAngle;
    RgbaColor   color;
    bool        filled;
    RgbaColor   fillColor;
    bool        visible;
    bool        exportable;
    StipplePattern stippleType;
    double      stippleScale;
    int         zIndex;

    // The default styles, for entities that don't have a style assigned yet,
    // and for datums and such.
    typedef struct {
        hStyle      h;
        const char *cnfPrefix;
        RgbaColor   color;
        double      width;
        int         zIndex;
    } Default;
    static const Default Defaults[];

    static std::string CnfColor(const std::string &prefix);
    static std::string CnfWidth(const std::string &prefix);
    static std::string CnfTextHeight(const std::string &prefix);
    static std::string CnfPrefixToName(const std::string &prefix);

    static void CreateAllDefaultStyles();
    static void CreateDefaultStyle(hStyle h);
    static void FillDefaultStyle(Style *s, const Default *d = NULL, bool factory = false);
    static void FreezeDefaultStyles();
    static void LoadFactoryDefaults();

    static void AssignSelectionToStyle(uint32_t v);
    static uint32_t CreateCustomStyle(bool rememberForUndo = true);

    static RgbaColor RewriteColor(RgbaColor rgb);

    static Style *Get(hStyle hs);
    static RgbaColor Color(hStyle hs, bool forExport=false);
    static RgbaColor Color(int hs, bool forExport=false);
    static RgbaColor FillColor(hStyle hs, bool forExport=false);
    static double Width(hStyle hs);
    static double Width(int hs);
    static double WidthMm(int hs);
    static double TextHeight(hStyle hs);
    static double DefaultTextHeight();
    static Canvas::Stroke Stroke(hStyle hs);
    static Canvas::Stroke Stroke(int hs);
    static bool Exportable(int hs);
    static hStyle ForEntity(hEntity he);
    static StipplePattern PatternType(hStyle hs);
    static double StippleScaleMm(hStyle hs);

    std::string DescriptionString() const;

    void Clear() {}
};


inline hEntity hGroup::entity(int i) const
    { hEntity r; r.v = 0x80000000 | (v << 16) | (uint32_t)i; return r; }
inline hParam hGroup::param(int i) const
    { hParam r; r.v = 0x80000000 | (v << 16) | (uint32_t)i; return r; }
inline hEquation hGroup::equation(int i) const
    { hEquation r; r.v = (v << 16) | 0x80000000 | (uint32_t)i; return r; }

inline bool hRequest::IsFromReferences() const {
    if(v == Request::HREQUEST_REFERENCE_XY.v) return true;
    if(v == Request::HREQUEST_REFERENCE_YZ.v) return true;
    if(v == Request::HREQUEST_REFERENCE_ZX.v) return true;
    return false;
}
inline hEntity hRequest::entity(int i) const
    { hEntity r; r.v = (v << 16) | (uint32_t)i; return r; }
inline hParam hRequest::param(int i) const
    { hParam r; r.v = (v << 16) | (uint32_t)i; return r; }

inline bool hEntity::isFromRequest() const
    { if(v & 0x80000000) return false; else return true; }
inline hRequest hEntity::request() const
    { hRequest r; r.v = (v >> 16); return r; }
inline hGroup hEntity::group() const
    { hGroup r; r.v = (v >> 16) & 0x3fff; return r; }
inline hEquation hEntity::equation(int i) const
    { hEquation r; r.v = v | 0x40000000 | (uint32_t)i; return r; }

inline hRequest hParam::request() const
    { hRequest r; r.v = (v >> 16); return r; }


inline hEquation hConstraint::equation(int i) const
    { hEquation r; r.v = (v << 16) | (uint32_t)i; return r; }
inline hParam hConstraint::param(int i) const
    { hParam r; r.v = v | 0x40000000 | (uint32_t)i; return r; }

inline bool hEquation::isFromConstraint() const
    { if(v & 0xc0000000) return false; else return true; }
inline hConstraint hEquation::constraint() const
    { hConstraint r; r.v = (v >> 16); return r; }

// The format for entities stored on the clipboard.
class ClipboardRequest {
public:
    Request::Type type;
    int         extraPoints;
    hStyle      style;
    std::string str;
    std::string font;
    Platform::Path file;
    bool        construction;

    Vector      point[MAX_POINTS_IN_ENTITY];
    double      distance;

    hEntity     oldEnt;
    hEntity     oldPointEnt[MAX_POINTS_IN_ENTITY];
    hRequest    newReq;
};

#endif
