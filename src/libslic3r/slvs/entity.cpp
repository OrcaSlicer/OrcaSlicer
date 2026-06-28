//-----------------------------------------------------------------------------
// The implementation of our entities in the symbolic algebra system, methods
// to return a symbolic representation of the entity (line by its endpoints,
// circle by center and radius, etc.).
//
// Copyright 2008-2013 Jonathan Westhues.
//-----------------------------------------------------------------------------
#include "solvespace.h"

const hEntity  EntityBase::FREE_IN_3D = { 0 };
const hEntity  EntityBase::NO_ENTITY = { 0 };

bool EntityBase::HasVector() const {
    switch(type) {
        case Type::LINE_SEGMENT:
        case Type::NORMAL_IN_3D:
        case Type::NORMAL_IN_2D:
        case Type::NORMAL_N_COPY:
        case Type::NORMAL_N_ROT:
        case Type::NORMAL_N_ROT_AA:
            return true;

        default:
            return false;
    }
}

ExprVector EntityBase::VectorGetExprsInWorkplane(hEntity wrkpl) const {
    switch(type) {
        case Type::LINE_SEGMENT:
            return (SK.GetEntity(point[0])->PointGetExprsInWorkplane(wrkpl)).Minus(
                    SK.GetEntity(point[1])->PointGetExprsInWorkplane(wrkpl));

        case Type::NORMAL_IN_3D:
        case Type::NORMAL_IN_2D:
        case Type::NORMAL_N_COPY:
        case Type::NORMAL_N_ROT:
        case Type::NORMAL_N_ROT_AA: {
            ExprVector ev = NormalExprsN();
            if(wrkpl.v == EntityBase::FREE_IN_3D.v) {
                return ev;
            }
            // Get the offset and basis vectors for this weird exotic csys.
            EntityBase *w = SK.GetEntity(wrkpl);
            ExprVector wu = w->Normal()->NormalExprsU();
            ExprVector wv = w->Normal()->NormalExprsV();

            // Get our coordinates in three-space, and project them into that
            // coordinate system.
            ExprVector result;
            result.x = ev.Dot(wu);
            result.y = ev.Dot(wv);
            result.z = Expr::From(0.0);
            return result;
        }
        default: ssassert(false, "Unexpected entity type");
    }
}

ExprVector EntityBase::VectorGetExprs() const {
    return VectorGetExprsInWorkplane(EntityBase::FREE_IN_3D);
}

Vector EntityBase::VectorGetNum() const {
    switch(type) {
        case Type::LINE_SEGMENT:
            return (SK.GetEntity(point[0])->PointGetNum()).Minus(
                    SK.GetEntity(point[1])->PointGetNum());

        case Type::NORMAL_IN_3D:
        case Type::NORMAL_IN_2D:
        case Type::NORMAL_N_COPY:
        case Type::NORMAL_N_ROT:
        case Type::NORMAL_N_ROT_AA:
            return NormalN();

        default: ssassert(false, "Unexpected entity type");
    }
}

Vector EntityBase::VectorGetRefPoint() const {
    switch(type) {
        case Type::LINE_SEGMENT:
            return ((SK.GetEntity(point[0])->PointGetNum()).Plus(
                     SK.GetEntity(point[1])->PointGetNum())).ScaledBy(0.5);

        case Type::NORMAL_IN_3D:
        case Type::NORMAL_IN_2D:
        case Type::NORMAL_N_COPY:
        case Type::NORMAL_N_ROT:
        case Type::NORMAL_N_ROT_AA:
            return SK.GetEntity(point[0])->PointGetNum();

        default: ssassert(false, "Unexpected entity type");
    }
}

Vector EntityBase::VectorGetStartPoint() const {
    switch(type) {
        case Type::LINE_SEGMENT:
            return SK.GetEntity(point[1])->PointGetNum();

        case Type::NORMAL_IN_3D:
        case Type::NORMAL_IN_2D:
        case Type::NORMAL_N_COPY:
        case Type::NORMAL_N_ROT:
        case Type::NORMAL_N_ROT_AA:
            return SK.GetEntity(point[0])->PointGetNum();

        default: ssassert(false, "Unexpected entity type");
    }
}

bool EntityBase::IsCircle() const {
    return (type == Type::CIRCLE) || (type == Type::ARC_OF_CIRCLE);
}

Expr *EntityBase::CircleGetRadiusExpr() const {
    if(type == Type::CIRCLE) {
        return SK.GetEntity(distance)->DistanceGetExpr();
    } else if(type == Type::ARC_OF_CIRCLE) {
        return Constraint::Distance(workplane, point[0], point[1]);
    } else ssassert(false, "Unexpected entity type");
}

double EntityBase::CircleGetRadiusNum() const {
    if(type == Type::CIRCLE) {
        return SK.GetEntity(distance)->DistanceGetNum();
    } else if(type == Type::ARC_OF_CIRCLE) {
        Vector c  = SK.GetEntity(point[0])->PointGetNum();
        Vector pa = SK.GetEntity(point[1])->PointGetNum();
        return (pa.Minus(c)).Magnitude();
    } else ssassert(false, "Unexpected entity type");
}

void EntityBase::ArcGetAngles(double *thetaa, double *thetab, double *dtheta) const {
    ssassert(type == Type::ARC_OF_CIRCLE, "Unexpected entity type");

    Quaternion q = Normal()->NormalGetNum();
    Vector u = q.RotationU(), v = q.RotationV();

    Vector c  = SK.GetEntity(point[0])->PointGetNum();
    Vector pa = SK.GetEntity(point[1])->PointGetNum();
    Vector pb = SK.GetEntity(point[2])->PointGetNum();

    Point2d c2  = c.Project2d(u, v);
    Point2d pa2 = (pa.Project2d(u, v)).Minus(c2);
    Point2d pb2 = (pb.Project2d(u, v)).Minus(c2);

    *thetaa = atan2(pa2.y, pa2.x);
    *thetab = atan2(pb2.y, pb2.x);
    *dtheta = *thetab - *thetaa;
    // If the endpoints are coincident, call it a full arc, not a zero arc;
    // useful concept to have when splitting
    while(*dtheta < 1e-6) *dtheta += 2*PI;
    while(*dtheta > (2*PI)) *dtheta -= 2*PI;
}

Vector EntityBase::CubicGetStartNum() const {
    return SK.GetEntity(point[0])->PointGetNum();
}
Vector EntityBase::CubicGetFinishNum() const {
    return SK.GetEntity(point[3+extraPoints])->PointGetNum();
}
ExprVector EntityBase::CubicGetStartTangentExprs() const {
    ExprVector pon  = SK.GetEntity(point[0])->PointGetExprs(),
               poff = SK.GetEntity(point[1])->PointGetExprs();
    return (pon.Minus(poff));
}
ExprVector EntityBase::CubicGetFinishTangentExprs() const {
    ExprVector pon  = SK.GetEntity(point[3+extraPoints])->PointGetExprs(),
               poff = SK.GetEntity(point[2+extraPoints])->PointGetExprs();
    return (pon.Minus(poff));
}
Vector EntityBase::CubicGetStartTangentNum() const {
    Vector pon  = SK.GetEntity(point[0])->PointGetNum(),
           poff = SK.GetEntity(point[1])->PointGetNum();
    return (pon.Minus(poff));
}
Vector EntityBase::CubicGetFinishTangentNum() const {
    Vector pon  = SK.GetEntity(point[3+extraPoints])->PointGetNum(),
           poff = SK.GetEntity(point[2+extraPoints])->PointGetNum();
    return (pon.Minus(poff));
}

bool EntityBase::IsWorkplane() const {
    return (type == Type::WORKPLANE);
}

ExprVector EntityBase::WorkplaneGetOffsetExprs() const {
    return SK.GetEntity(point[0])->PointGetExprs();
}

Vector EntityBase::WorkplaneGetOffset() const {
    return SK.GetEntity(point[0])->PointGetNum();
}

void EntityBase::WorkplaneGetPlaneExprs(ExprVector *n, Expr **dn) const {
    if(type == Type::WORKPLANE) {
        *n = Normal()->NormalExprsN();

        ExprVector p0 = SK.GetEntity(point[0])->PointGetExprs();
        // The plane is n dot (p - p0) = 0, or
        //              n dot p - n dot p0 = 0
        // so dn = n dot p0
        *dn = p0.Dot(*n);
    } else ssassert(false, "Unexpected entity type");
}

bool EntityBase::IsDistance() const {
    return (type == Type::DISTANCE) ||
           (type == Type::DISTANCE_N_COPY);
}
double EntityBase::DistanceGetNum() const {
    if(type == Type::DISTANCE) {
        return SK.GetParam(param[0])->val;
    } else if(type == Type::DISTANCE_N_COPY) {
        return numDistance;
    } else ssassert(false, "Unexpected entity type");
}
Expr *EntityBase::DistanceGetExpr() const {
    if(type == Type::DISTANCE) {
        return Expr::From(param[0]);
    } else if(type == Type::DISTANCE_N_COPY) {
        return Expr::From(numDistance);
    } else ssassert(false, "Unexpected entity type");
}
void EntityBase::DistanceForceTo(double v) {
    if(type == Type::DISTANCE) {
        (SK.GetParam(param[0]))->val = v;
    } else if(type == Type::DISTANCE_N_COPY) {
        // do nothing, it's locked
    } else ssassert(false, "Unexpected entity type");
}

EntityBase *EntityBase::Normal() const {
    return SK.GetEntity(normal);
}

bool EntityBase::IsPoint() const {
    switch(type) {
        case Type::POINT_IN_3D:
        case Type::POINT_IN_2D:
        case Type::POINT_N_COPY:
        case Type::POINT_N_TRANS:
        case Type::POINT_N_ROT_TRANS:
        case Type::POINT_N_ROT_AA:
            return true;

        default:
            return false;
    }
}

bool EntityBase::IsNormal() const {
    switch(type) {
        case Type::NORMAL_IN_3D:
        case Type::NORMAL_IN_2D:
        case Type::NORMAL_N_COPY:
        case Type::NORMAL_N_ROT:
        case Type::NORMAL_N_ROT_AA:
            return true;

        default:           return false;
    }
}

Quaternion EntityBase::NormalGetNum() const {
    Quaternion q;
    switch(type) {
        case Type::NORMAL_IN_3D:
            q = Quaternion::From(param[0], param[1], param[2], param[3]);
            break;

        case Type::NORMAL_IN_2D: {
            EntityBase *wrkpl = SK.GetEntity(workplane);
            EntityBase *norm = SK.GetEntity(wrkpl->normal);
            q = norm->NormalGetNum();
            break;
        }
        case Type::NORMAL_N_COPY:
            q = numNormal;
            break;

        case Type::NORMAL_N_ROT:
            q = Quaternion::From(param[0], param[1], param[2], param[3]);
            q = q.Times(numNormal);
            break;

        case Type::NORMAL_N_ROT_AA: {
            q = GetAxisAngleQuaternion(0);
            q = q.Times(numNormal);
            break;
        }

        default: ssassert(false, "Unexpected entity type");
    }
    return q;
}

void EntityBase::NormalForceTo(Quaternion q) {
    switch(type) {
        case Type::NORMAL_IN_3D:
            SK.GetParam(param[0])->val = q.w;
            SK.GetParam(param[1])->val = q.vx;
            SK.GetParam(param[2])->val = q.vy;
            SK.GetParam(param[3])->val = q.vz;
            break;

        case Type::NORMAL_IN_2D:
        case Type::NORMAL_N_COPY:
            // There's absolutely nothing to do; these are locked.
            break;
        case Type::NORMAL_N_ROT: {
            Quaternion qp = q.Times(numNormal.Inverse());

            SK.GetParam(param[0])->val = qp.w;
            SK.GetParam(param[1])->val = qp.vx;
            SK.GetParam(param[2])->val = qp.vy;
            SK.GetParam(param[3])->val = qp.vz;
            break;
        }

        case Type::NORMAL_N_ROT_AA:
            // Not sure if I'll bother implementing this one
            break;

        default: ssassert(false, "Unexpected entity type");
    }
}

Vector EntityBase::NormalU() const {
    return NormalGetNum().RotationU();
}
Vector EntityBase::NormalV() const {
    return NormalGetNum().RotationV();
}
Vector EntityBase::NormalN() const {
    return NormalGetNum().RotationN();
}

ExprVector EntityBase::NormalExprsU() const {
    return NormalGetExprs().RotationU();
}
ExprVector EntityBase::NormalExprsV() const {
    return NormalGetExprs().RotationV();
}
ExprVector EntityBase::NormalExprsN() const {
    return NormalGetExprs().RotationN();
}

ExprQuaternion EntityBase::NormalGetExprs() const {
    ExprQuaternion q;
    switch(type) {
        case Type::NORMAL_IN_3D:
            q = ExprQuaternion::From(param[0], param[1], param[2], param[3]);
            break;

        case Type::NORMAL_IN_2D: {
            EntityBase *wrkpl = SK.GetEntity(workplane);
            EntityBase *norm = SK.GetEntity(wrkpl->normal);
            q = norm->NormalGetExprs();
            break;
        }
        case Type::NORMAL_N_COPY:
            q = ExprQuaternion::From(numNormal);
            break;

        case Type::NORMAL_N_ROT: {
            ExprQuaternion orig = ExprQuaternion::From(numNormal);
            q = ExprQuaternion::From(param[0], param[1], param[2], param[3]);

            q = q.Times(orig);
            break;
        }

        case Type::NORMAL_N_ROT_AA: {
            ExprQuaternion orig = ExprQuaternion::From(numNormal);
            q = GetAxisAngleQuaternionExprs(0);
            q = q.Times(orig);
            break;
        }

        default: ssassert(false, "Unexpected entity type");
    }
    return q;
}

void EntityBase::PointForceParamTo(Vector p) {
    switch(type) {
        case Type::POINT_IN_3D:
            SK.GetParam(param[0])->val = p.x;
            SK.GetParam(param[1])->val = p.y;
            SK.GetParam(param[2])->val = p.z;
            break;

        case Type::POINT_IN_2D:
            SK.GetParam(param[0])->val = p.x;
            SK.GetParam(param[1])->val = p.y;
            break;

        default: ssassert(false, "Unexpected entity type");
    }
}

void EntityBase::PointForceTo(Vector p) {
    switch(type) {
        case Type::POINT_IN_3D:
            SK.GetParam(param[0])->val = p.x;
            SK.GetParam(param[1])->val = p.y;
            SK.GetParam(param[2])->val = p.z;
            break;

        case Type::POINT_IN_2D: {
            EntityBase *c = SK.GetEntity(workplane);
            p = p.Minus(c->WorkplaneGetOffset());
            SK.GetParam(param[0])->val = p.Dot(c->Normal()->NormalU());
            SK.GetParam(param[1])->val = p.Dot(c->Normal()->NormalV());
            break;
        }

        case Type::POINT_N_TRANS: {
            if(timesApplied == 0) break;
            Vector trans = (p.Minus(numPoint)).ScaledBy(1.0/timesApplied);
            SK.GetParam(param[0])->val = trans.x;
            SK.GetParam(param[1])->val = trans.y;
            SK.GetParam(param[2])->val = trans.z;
            break;
        }

        case Type::POINT_N_ROT_TRANS: {
            // Force only the translation; leave the rotation unchanged. But
            // remember that we're working with respect to the rotated
            // point.
            Vector trans = p.Minus(PointGetQuaternion().Rotate(numPoint));
            SK.GetParam(param[0])->val = trans.x;
            SK.GetParam(param[1])->val = trans.y;
            SK.GetParam(param[2])->val = trans.z;
            break;
        }

        case Type::POINT_N_ROT_AA: {
            // Force only the angle; the axis and center of rotation stay
            Vector offset = Vector::From(param[0], param[1], param[2]);
            Vector normal = Vector::From(param[4], param[5], param[6]);
            Vector u = normal.Normal(0), v = normal.Normal(1);
            Vector po = p.Minus(offset), numo = numPoint.Minus(offset);
            double thetap = atan2(v.Dot(po), u.Dot(po));
            double thetan = atan2(v.Dot(numo), u.Dot(numo));
            double thetaf = (thetap - thetan);
            double thetai = (SK.GetParam(param[3])->val)*timesApplied*2;
            double dtheta = thetaf - thetai;
            // Take the smallest possible change in the actual step angle,
            // in order to avoid jumps when you cross from +pi to -pi
            while(dtheta < -PI) dtheta += 2*PI;
            while(dtheta > PI) dtheta -= 2*PI;
            SK.GetParam(param[3])->val = (thetai + dtheta)/(timesApplied*2);
            break;
        }

        case Type::POINT_N_COPY:
            // Nothing to do; it's a static copy
            break;

        default: ssassert(false, "Unexpected entity type");
    }
}

Vector EntityBase::PointGetNum() const {
    Vector p;
    switch(type) {
        case Type::POINT_IN_3D:
            p = Vector::From(param[0], param[1], param[2]);
            break;

        case Type::POINT_IN_2D: {
            EntityBase *c = SK.GetEntity(workplane);
            Vector u = c->Normal()->NormalU();
            Vector v = c->Normal()->NormalV();
            p =        u.ScaledBy(SK.GetParam(param[0])->val);
            p = p.Plus(v.ScaledBy(SK.GetParam(param[1])->val));
            p = p.Plus(c->WorkplaneGetOffset());
            break;
        }

        case Type::POINT_N_TRANS: {
            Vector trans = Vector::From(param[0], param[1], param[2]);
            p = numPoint.Plus(trans.ScaledBy(timesApplied));
            break;
        }

        case Type::POINT_N_ROT_TRANS: {
            Vector offset = Vector::From(param[0], param[1], param[2]);
            Quaternion q = PointGetQuaternion();
            p = q.Rotate(numPoint);
            p = p.Plus(offset);
            break;
        }

        case Type::POINT_N_ROT_AA: {
            Vector offset = Vector::From(param[0], param[1], param[2]);
            Quaternion q = PointGetQuaternion();
            p = numPoint.Minus(offset);
            p = q.Rotate(p);
            p = p.Plus(offset);
            break;
        }

        case Type::POINT_N_COPY:
            p = numPoint;
            break;

        default: ssassert(false, "Unexpected entity type");
    }
    return p;
}

ExprVector EntityBase::PointGetExprs() const {
    ExprVector r;
    switch(type) {
        case Type::POINT_IN_3D:
            r = ExprVector::From(param[0], param[1], param[2]);
            break;

        case Type::POINT_IN_2D: {
            EntityBase *c = SK.GetEntity(workplane);
            ExprVector u = c->Normal()->NormalExprsU();
            ExprVector v = c->Normal()->NormalExprsV();
            r = c->WorkplaneGetOffsetExprs();
            r = r.Plus(u.ScaledBy(Expr::From(param[0])));
            r = r.Plus(v.ScaledBy(Expr::From(param[1])));
            break;
        }
        case Type::POINT_N_TRANS: {
            ExprVector orig = ExprVector::From(numPoint);
            ExprVector trans = ExprVector::From(param[0], param[1], param[2]);
            r = orig.Plus(trans.ScaledBy(Expr::From(timesApplied)));
            break;
        }
        case Type::POINT_N_ROT_TRANS: {
            ExprVector orig = ExprVector::From(numPoint);
            ExprVector trans = ExprVector::From(param[0], param[1], param[2]);
            ExprQuaternion q =
                ExprQuaternion::From(param[3], param[4], param[5], param[6]);
            orig = q.Rotate(orig);
            r = orig.Plus(trans);
            break;
        }
        case Type::POINT_N_ROT_AA: {
            ExprVector orig = ExprVector::From(numPoint);
            ExprVector trans = ExprVector::From(param[0], param[1], param[2]);
            ExprQuaternion q = GetAxisAngleQuaternionExprs(3);
            orig = orig.Minus(trans);
            orig = q.Rotate(orig);
            r = orig.Plus(trans);
            break;
        }
        case Type::POINT_N_COPY:
            r = ExprVector::From(numPoint);
            break;

        default: ssassert(false, "Unexpected entity type");
    }
    return r;
}

void EntityBase::PointGetExprsInWorkplane(hEntity wrkpl, Expr **u, Expr **v) const {
    if(type == Type::POINT_IN_2D && workplane.v == wrkpl.v) {
        // They want our coordinates in the form that we've written them,
        // very nice.
        *u = Expr::From(param[0]);
        *v = Expr::From(param[1]);
    } else {
        // Get the offset and basis vectors for this weird exotic csys.
        EntityBase *w = SK.GetEntity(wrkpl);
        ExprVector wp = w->WorkplaneGetOffsetExprs();
        ExprVector wu = w->Normal()->NormalExprsU();
        ExprVector wv = w->Normal()->NormalExprsV();

        // Get our coordinates in three-space, and project them into that
        // coordinate system.
        ExprVector ev = PointGetExprs();
        ev = ev.Minus(wp);
        *u = ev.Dot(wu);
        *v = ev.Dot(wv);
    }
}

ExprVector EntityBase::PointGetExprsInWorkplane(hEntity wrkpl) const {
    if(wrkpl.v == Entity::FREE_IN_3D.v) {
        return PointGetExprs();
    }

    ExprVector r;
    PointGetExprsInWorkplane(wrkpl, &r.x, &r.y);
    r.z = Expr::From(0.0);
    return r;
}

void EntityBase::PointForceQuaternionTo(Quaternion q) {
    ssassert(type == Type::POINT_N_ROT_TRANS, "Unexpected entity type");

    SK.GetParam(param[3])->val = q.w;
    SK.GetParam(param[4])->val = q.vx;
    SK.GetParam(param[5])->val = q.vy;
    SK.GetParam(param[6])->val = q.vz;
}

Quaternion EntityBase::GetAxisAngleQuaternion(int param0) const {
    Quaternion q;
    double theta = timesApplied*SK.GetParam(param[param0+0])->val;
    double s = sin(theta), c = cos(theta);
    q.w = c;
    q.vx = s*SK.GetParam(param[param0+1])->val;
    q.vy = s*SK.GetParam(param[param0+2])->val;
    q.vz = s*SK.GetParam(param[param0+3])->val;
    return q;
}

ExprQuaternion EntityBase::GetAxisAngleQuaternionExprs(int param0) const {
    ExprQuaternion q;

    Expr *theta = Expr::From(timesApplied)->Times(
                  Expr::From(param[param0+0]));
    Expr *c = theta->Cos(), *s = theta->Sin();
    q.w = c;
    q.vx = s->Times(Expr::From(param[param0+1]));
    q.vy = s->Times(Expr::From(param[param0+2]));
    q.vz = s->Times(Expr::From(param[param0+3]));
    return q;
}

Quaternion EntityBase::PointGetQuaternion() const {
    Quaternion q;

    if(type == Type::POINT_N_ROT_AA) {
        q = GetAxisAngleQuaternion(3);
    } else if(type == Type::POINT_N_ROT_TRANS) {
        q = Quaternion::From(param[3], param[4], param[5], param[6]);
    } else ssassert(false, "Unexpected entity type");

    return q;
}

bool EntityBase::IsFace() const {
    switch(type) {
        case Type::FACE_NORMAL_PT:
        case Type::FACE_XPROD:
        case Type::FACE_N_ROT_TRANS:
        case Type::FACE_N_TRANS:
        case Type::FACE_N_ROT_AA:
            return true;
        default:
            return false;
    }
}

ExprVector EntityBase::FaceGetNormalExprs() const {
    ExprVector r;
    if(type == Type::FACE_NORMAL_PT) {
        Vector v = Vector::From(numNormal.vx, numNormal.vy, numNormal.vz);
        r = ExprVector::From(v.WithMagnitude(1));
    } else if(type == Type::FACE_XPROD) {
        ExprVector vc = ExprVector::From(param[0], param[1], param[2]);
        ExprVector vn =
            ExprVector::From(numNormal.vx, numNormal.vy, numNormal.vz);
        r = vc.Cross(vn);
        r = r.WithMagnitude(Expr::From(1.0));
    } else if(type == Type::FACE_N_ROT_TRANS) {
        // The numerical normal vector gets the rotation; the numerical
        // normal has magnitude one, and the rotation doesn't change that,
        // so there's no need to fix it up.
        r = ExprVector::From(numNormal.vx, numNormal.vy, numNormal.vz);
        ExprQuaternion q =
            ExprQuaternion::From(param[3], param[4], param[5], param[6]);
        r = q.Rotate(r);
    } else if(type == Type::FACE_N_TRANS) {
        r = ExprVector::From(numNormal.vx, numNormal.vy, numNormal.vz);
    } else if(type == Type::FACE_N_ROT_AA) {
        r = ExprVector::From(numNormal.vx, numNormal.vy, numNormal.vz);
        ExprQuaternion q = GetAxisAngleQuaternionExprs(3);
        r = q.Rotate(r);
    } else ssassert(false, "Unexpected entity type");
    return r;
}

Vector EntityBase::FaceGetNormalNum() const {
    Vector r;
    if(type == Type::FACE_NORMAL_PT) {
        r = Vector::From(numNormal.vx, numNormal.vy, numNormal.vz);
    } else if(type == Type::FACE_XPROD) {
        Vector vc = Vector::From(param[0], param[1], param[2]);
        Vector vn = Vector::From(numNormal.vx, numNormal.vy, numNormal.vz);
        r = vc.Cross(vn);
    } else if(type == Type::FACE_N_ROT_TRANS) {
        // The numerical normal vector gets the rotation
        r = Vector::From(numNormal.vx, numNormal.vy, numNormal.vz);
        Quaternion q = Quaternion::From(param[3], param[4], param[5], param[6]);
        r = q.Rotate(r);
    } else if(type == Type::FACE_N_TRANS) {
        r = Vector::From(numNormal.vx, numNormal.vy, numNormal.vz);
    } else if(type == Type::FACE_N_ROT_AA) {
        r = Vector::From(numNormal.vx, numNormal.vy, numNormal.vz);
        Quaternion q = GetAxisAngleQuaternion(3);
        r = q.Rotate(r);
    } else ssassert(false, "Unexpected entity type");
    return r.WithMagnitude(1);
}

ExprVector EntityBase::FaceGetPointExprs() const {
    ExprVector r;
    if(type == Type::FACE_NORMAL_PT) {
        r = SK.GetEntity(point[0])->PointGetExprs();
    } else if(type == Type::FACE_XPROD) {
        r = ExprVector::From(numPoint);
    } else if(type == Type::FACE_N_ROT_TRANS) {
        // The numerical point gets the rotation and translation.
        ExprVector trans = ExprVector::From(param[0], param[1], param[2]);
        ExprQuaternion q =
            ExprQuaternion::From(param[3], param[4], param[5], param[6]);
        r = ExprVector::From(numPoint);
        r = q.Rotate(r);
        r = r.Plus(trans);
    } else if(type == Type::FACE_N_TRANS) {
        ExprVector trans = ExprVector::From(param[0], param[1], param[2]);
        r = ExprVector::From(numPoint);
        r = r.Plus(trans.ScaledBy(Expr::From(timesApplied)));
    } else if(type == Type::FACE_N_ROT_AA) {
        ExprVector trans = ExprVector::From(param[0], param[1], param[2]);
        ExprQuaternion q = GetAxisAngleQuaternionExprs(3);
        r = ExprVector::From(numPoint);
        r = r.Minus(trans);
        r = q.Rotate(r);
        r = r.Plus(trans);
    } else ssassert(false, "Unexpected entity type");
    return r;
}

Vector EntityBase::FaceGetPointNum() const {
    Vector r;
    if(type == Type::FACE_NORMAL_PT) {
        r = SK.GetEntity(point[0])->PointGetNum();
    } else if(type == Type::FACE_XPROD) {
        r = numPoint;
    } else if(type == Type::FACE_N_ROT_TRANS) {
        // The numerical point gets the rotation and translation.
        Vector trans = Vector::From(param[0], param[1], param[2]);
        Quaternion q = Quaternion::From(param[3], param[4], param[5], param[6]);
        r = q.Rotate(numPoint);
        r = r.Plus(trans);
    } else if(type == Type::FACE_N_TRANS) {
        Vector trans = Vector::From(param[0], param[1], param[2]);
        r = numPoint.Plus(trans.ScaledBy(timesApplied));
    } else if(type == Type::FACE_N_ROT_AA) {
        Vector trans = Vector::From(param[0], param[1], param[2]);
        Quaternion q = GetAxisAngleQuaternion(3);
        r = numPoint.Minus(trans);
        r = q.Rotate(r);
        r = r.Plus(trans);
    } else ssassert(false, "Unexpected entity type");
    return r;
}

bool EntityBase::HasEndpoints() const {
    return (type == Type::LINE_SEGMENT) ||
           (type == Type::CUBIC) ||
           (type == Type::ARC_OF_CIRCLE);
}
Vector EntityBase::EndpointStart() const {
    if(type == Type::LINE_SEGMENT) {
        return SK.GetEntity(point[0])->PointGetNum();
    } else if(type == Type::CUBIC) {
        return CubicGetStartNum();
    } else if(type == Type::ARC_OF_CIRCLE) {
        return SK.GetEntity(point[1])->PointGetNum();
    } else ssassert(false, "Unexpected entity type");
}
Vector EntityBase::EndpointFinish() const {
    if(type == Type::LINE_SEGMENT) {
        return SK.GetEntity(point[1])->PointGetNum();
    } else if(type == Type::CUBIC) {
        return CubicGetFinishNum();
    } else if(type == Type::ARC_OF_CIRCLE) {
        return SK.GetEntity(point[2])->PointGetNum();
    } else ssassert(false, "Unexpected entity type");
}

void EntityBase::RectGetPointsExprs(ExprVector *eb, ExprVector *ec) const {
    ssassert(type == Type::TTF_TEXT || type == Type::IMAGE,
             "Unexpected entity type");

    EntityBase *a = SK.GetEntity(point[0]);
    EntityBase *o = SK.GetEntity(point[1]);

    // Write equations for each point in the current workplane.
    // This reduces the complexity of resulting equations.
    ExprVector ea = a->PointGetExprsInWorkplane(workplane);
    ExprVector eo = o->PointGetExprsInWorkplane(workplane);

    // Take perpendicular vector and scale it by aspect ratio.
    ExprVector eu = ea.Minus(eo);
    ExprVector ev = ExprVector::From(eu.y, eu.x->Negate(), eu.z).ScaledBy(Expr::From(aspectRatio));

    *eb = eo.Plus(ev);
    *ec = eo.Plus(eu).Plus(ev);
}

void EntityBase::AddEq(IdList<Equation,hEquation> *l, Expr *expr, int index) const {
    Equation eq;
    eq.e = expr;
    eq.h = h.equation(index);
    l->Add(&eq);
}

void EntityBase::GenerateEquations(IdList<Equation,hEquation> *l) const {
    switch(type) {
        case Type::NORMAL_IN_3D: {
            ExprQuaternion q = NormalGetExprs();
            AddEq(l, (q.Magnitude())->Minus(Expr::From(1)), 0);
            break;
        }

        case Type::ARC_OF_CIRCLE: {
            // If this is a copied entity, with its point already fixed
            // with respect to each other, then we don't want to generate
            // the distance constraint!
            if(SK.GetEntity(point[0])->type != Type::POINT_IN_2D) break;

            // If the two endpoints of the arc are constrained coincident
            // (to make a complete circle), then our distance constraint
            // would be redundant and therefore overconstrain things.
            int i;
            for(i = 0; i < SK.constraint.n; i++) {
                ConstraintBase *c = &(SK.constraint.elem[i]);
                if(c->group.v != group.v) continue;
                if(c->type != Constraint::Type::POINTS_COINCIDENT) continue;

                if((c->ptA.v == point[1].v && c->ptB.v == point[2].v) ||
                   (c->ptA.v == point[2].v && c->ptB.v == point[1].v))
                {
                    break;
                }
            }
            if(i < SK.constraint.n) break;

            Expr *ra = Constraint::Distance(workplane, point[0], point[1]);
            Expr *rb = Constraint::Distance(workplane, point[0], point[2]);
            AddEq(l, ra->Minus(rb), 0);
            break;
        }

        case Type::IMAGE:
        case Type::TTF_TEXT: {
            if(SK.GetEntity(point[0])->type != Type::POINT_IN_2D) break;
            EntityBase *b = SK.GetEntity(point[2]);
            EntityBase *c = SK.GetEntity(point[3]);
            ExprVector eb = b->PointGetExprsInWorkplane(workplane);
            ExprVector ec = c->PointGetExprsInWorkplane(workplane);

            ExprVector ebp, ecp;
            RectGetPointsExprs(&ebp, &ecp);

            ExprVector beq = eb.Minus(ebp);
            AddEq(l, beq.x, 0);
            AddEq(l, beq.y, 1);
            ExprVector ceq = ec.Minus(ecp);
            AddEq(l, ceq.x, 2);
            AddEq(l, ceq.y, 3);
            break;
        }

        default: // Most entities do not generate equations.
            break;
    }
}
