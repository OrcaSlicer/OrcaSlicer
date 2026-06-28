//-----------------------------------------------------------------------------
// An expression in our symbolic algebra system, used to write, linearize,
// and solve our constraint equations.
//
// Copyright 2008-2013 Jonathan Westhues.
//-----------------------------------------------------------------------------
#ifndef __EXPR_H
#define __EXPR_H

class Expr {
public:

    enum class Op : uint32_t {
        // A parameter, by the hParam handle
        PARAM          =  0,
        // A parameter, by a pointer straight in to the param table (faster,
        // if we know that the param table won't move around)
        PARAM_PTR      =  1,

        // Operands
        CONSTANT       = 20,
        VARIABLE       = 21,

        // Binary ops
        PLUS           = 100,
        MINUS          = 101,
        TIMES          = 102,
        DIV            = 103,
        // Unary ops
        NEGATE         = 104,
        SQRT           = 105,
        SQUARE         = 106,
        SIN            = 107,
        COS            = 108,
        ASIN           = 109,
        ACOS           = 110,
    };

    Op      op;
    Expr    *a;
    union {
        double  v;
        hParam  parh;
        Param  *parp;
        Expr    *b;
    };

    Expr() { }
    Expr(double val) : op(Op::CONSTANT) { v = val; }

    static inline Expr *AllocExpr()
        { return (Expr *)AllocTemporary(sizeof(Expr)); }

    static Expr *From(hParam p);
    static Expr *From(double v);

    Expr *AnyOp(Op op, Expr *b);
    inline Expr *Plus (Expr *b_) { return AnyOp(Op::PLUS,  b_); }
    inline Expr *Minus(Expr *b_) { return AnyOp(Op::MINUS, b_); }
    inline Expr *Times(Expr *b_) { return AnyOp(Op::TIMES, b_); }
    inline Expr *Div  (Expr *b_) { return AnyOp(Op::DIV,   b_); }

    inline Expr *Negate() { return AnyOp(Op::NEGATE, NULL); }
    inline Expr *Sqrt  () { return AnyOp(Op::SQRT,   NULL); }
    inline Expr *Square() { return AnyOp(Op::SQUARE, NULL); }
    inline Expr *Sin   () { return AnyOp(Op::SIN,    NULL); }
    inline Expr *Cos   () { return AnyOp(Op::COS,    NULL); }
    inline Expr *ASin  () { return AnyOp(Op::ASIN,   NULL); }
    inline Expr *ACos  () { return AnyOp(Op::ACOS,   NULL); }

    Expr *PartialWrt(hParam p) const;
    double Eval() const;
    uint64_t ParamsUsed() const;
    bool DependsOn(hParam p) const;
    static bool Tol(double a, double b);
    Expr *FoldConstants();
    void Substitute(hParam oldh, hParam newh);

    static const hParam NO_PARAMS, MULTIPLE_PARAMS;
    hParam ReferencedParams(ParamList *pl) const;

    void ParamsToPointers();

    std::string Print() const;

    // number of child nodes: 0 (e.g. constant), 1 (sqrt), or 2 (+)
    int Children() const;
    // total number of nodes in the tree
    int Nodes() const;

    // Make a simple copy
    Expr *DeepCopy() const;
    // Make a copy, with the parameters (usually referenced by hParam)
    // resolved to pointers to the actual value. This speeds things up
    // considerably.
    Expr *DeepCopyWithParamsAsPointers(IdList<Param,hParam> *firstTry,
                                       IdList<Param,hParam> *thenTry) const;

    static Expr *Parse(const char *input, std::string *error);
    static Expr *From(const char *in, bool popUpError);
};

class ExprVector {
public:
    Expr *x, *y, *z;

    static ExprVector From(Expr *x, Expr *y, Expr *z);
    static ExprVector From(Vector vn);
    static ExprVector From(hParam x, hParam y, hParam z);
    static ExprVector From(double x, double y, double z);

    ExprVector Plus(ExprVector b) const;
    ExprVector Minus(ExprVector b) const;
    Expr *Dot(ExprVector b) const;
    ExprVector Cross(ExprVector b) const;
    ExprVector ScaledBy(Expr *s) const;
    ExprVector WithMagnitude(Expr *s) const;
    Expr *Magnitude() const;

    Vector Eval() const;
};

class ExprQuaternion {
public:
    Expr *w, *vx, *vy, *vz;

    static ExprQuaternion From(Expr *w, Expr *vx, Expr *vy, Expr *vz);
    static ExprQuaternion From(Quaternion qn);
    static ExprQuaternion From(hParam w, hParam vx, hParam vy, hParam vz);

    ExprVector RotationU() const;
    ExprVector RotationV() const;
    ExprVector RotationN() const;

    ExprVector Rotate(ExprVector p) const;
    ExprQuaternion Times(ExprQuaternion b) const;

    Expr *Magnitude() const;
};
#endif
