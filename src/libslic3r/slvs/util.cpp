//-----------------------------------------------------------------------------
// Utility functions, mostly various kinds of vector math (working on real
// numbers, not working on quantities in the symbolic algebra system).
//
// Copyright 2008-2013 Jonathan Westhues.
//-----------------------------------------------------------------------------
#define _USE_MATH_DEFINES
#include "solvespace.h"

#include <cmath>

std::string SolveSpace::ssprintf(const char *fmt, ...)
{
    va_list va;

    va_start(va, fmt);
    int size = vsnprintf(NULL, 0, fmt, va);
    ssassert(size >= 0, "vsnprintf could not encode string");
    va_end(va);

    std::string result;
    result.resize(size + 1);

    va_start(va, fmt);
    vsnprintf(&result[0], size + 1, fmt, va);
    va_end(va);

    result.resize(size);
    return result;
}
#if FULL_LIB_JJS

char32_t utf8_iterator::operator*()
{
    const uint8_t *it = (const uint8_t*) this->p;
    char32_t result = *it;

    if((result & 0x80) != 0) {
      unsigned int mask = 0x40;

      do {
        result <<= 6;
        unsigned int c = (*++it);
        mask   <<= 5;
        result  += c - 0x80;
      } while((result & mask) != 0);

      result &= mask - 1;
    }

    this->n = (const char*) (it + 1);
    return result;
}
int64_t SolveSpace::GetMilliseconds()
{
    auto timestamp = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(timestamp).count();
}
#endif

void SolveSpace::MakeMatrix(double *mat,
                            double a11, double a12, double a13, double a14,
                            double a21, double a22, double a23, double a24,
                            double a31, double a32, double a33, double a34,
                            double a41, double a42, double a43, double a44)
{
    mat[ 0] = a11;
    mat[ 1] = a21;
    mat[ 2] = a31;
    mat[ 3] = a41;
    mat[ 4] = a12;
    mat[ 5] = a22;
    mat[ 6] = a32;
    mat[ 7] = a42;
    mat[ 8] = a13;
    mat[ 9] = a23;
    mat[10] = a33;
    mat[11] = a43;
    mat[12] = a14;
    mat[13] = a24;
    mat[14] = a34;
    mat[15] = a44;
}

void SolveSpace::MultMatrix(double *mata, double *matb, double *matr) {
    for(int i = 0; i < 4; i++) {
        for(int j = 0; j < 4; j++) {
            double s = 0.0;
            for(int k = 0; k < 4; k++) {
                s += mata[k * 4 + j] * matb[i * 4 + k];
            }
           matr[i * 4 + j] = s;
        }
    }
}

//-----------------------------------------------------------------------------
// Word-wrap the string for our message box appropriately, and then display
// that string.
//-----------------------------------------------------------------------------
static void DoStringForMessageBox(const char *str, va_list f, bool error)
{
    char inBuf[1024*50];
    vsprintf(inBuf, str, f);

    char outBuf[1024*50];
    int i = 0, j = 0, len = 0, longestLen = 47;
    int rows = 0, cols = 0;

    // Count the width of the longest line that starts with spaces; those
    // are list items, that should not be split in the middle.
    bool listLine = false;
    while(inBuf[i]) {
        if(inBuf[i] == '\r') {
            // ignore these
        } else if(inBuf[i] == ' ' && len == 0) {
            listLine = true;
        } else if(inBuf[i] == '\n') {
            if(listLine) longestLen = max(longestLen, len);
            len = 0;
        } else {
            len++;
        }
        i++;
    }
    if(listLine) longestLen = max(longestLen, len);

    // Word wrap according to our target line length longestLen.
    len = 0;
    i = 0;
    while(inBuf[i]) {
        if(inBuf[i] == '\r') {
            // ignore these
        } else if(inBuf[i] == '\n') {
            outBuf[j++] = '\n';
            if(len == 0) rows++;
            len = 0;
        } else if(inBuf[i] == ' ' && len > longestLen) {
            outBuf[j++] = '\n';
            len = 0;
        } else {
            outBuf[j++] = inBuf[i];
            // Count rows when we draw the first character; so an empty
            // row doesn't end up counting.
            if(len == 0) rows++;
            len++;
        }
        cols = max(cols, len);
        i++;
    }
    outBuf[j++] = '\0';

    // And then display the text with our actual longest line length.
    DoMessageBox(outBuf, rows, cols, error);
}
void SolveSpace::Error(const char *str, ...)
{
    va_list f;
    va_start(f, str);
    DoStringForMessageBox(str, f, /*error=*/true);
    va_end(f);
}
void SolveSpace::Message(const char *str, ...)
{
    va_list f;
    va_start(f, str);
    DoStringForMessageBox(str, f, /*error=*/false);
    va_end(f);
}

void SolveSpace::CnfFreezeBool(bool v, const std::string &name)
    { CnfFreezeInt(v ? 1 : 0, name); }

void SolveSpace::CnfFreezeColor(RgbaColor v, const std::string &name)
    { CnfFreezeInt(v.ToPackedInt(), name); }

bool SolveSpace::CnfThawBool(bool v, const std::string &name)
    { return CnfThawInt(v ? 1 : 0, name) != 0; }

RgbaColor SolveSpace::CnfThawColor(RgbaColor v, const std::string &name)
    { return RgbaColor::FromPackedInt(CnfThawInt(v.ToPackedInt(), name)); }

//-----------------------------------------------------------------------------
// Solve a mostly banded matrix. In a given row, there are LEFT_OF_DIAG
// elements to the left of the diagonal element, and RIGHT_OF_DIAG elements to
// the right (so that the total band width is LEFT_OF_DIAG + RIGHT_OF_DIAG + 1).
// There also may be elements in the last two columns of any row. We solve
// without pivoting.
//-----------------------------------------------------------------------------
void BandedMatrix::Solve() {
    int i, ip, j, jp;
    double temp;

    // Reduce the matrix to upper triangular form.
    for(i = 0; i < n; i++) {
        for(ip = i+1; ip < n && ip <= (i + LEFT_OF_DIAG); ip++) {
            temp = A[ip][i]/A[i][i];

            for(jp = i; jp < (n - 2) && jp <= (i + RIGHT_OF_DIAG); jp++) {
                A[ip][jp] -= temp*(A[i][jp]);
            }
            A[ip][n-2] -= temp*(A[i][n-2]);
            A[ip][n-1] -= temp*(A[i][n-1]);

            B[ip] -= temp*B[i];
        }
    }

    // And back-substitute.
    for(i = n - 1; i >= 0; i--) {
        temp = B[i];

        if(i < n-1) temp -= X[n-1]*A[i][n-1];
        if(i < n-2) temp -= X[n-2]*A[i][n-2];

        for(j = min(n - 3, i + RIGHT_OF_DIAG); j > i; j--) {
            temp -= X[j]*A[i][j];
        }
        X[i] = temp / A[i][i];
    }
}

const Quaternion Quaternion::IDENTITY = { 1, 0, 0, 0 };

Quaternion Quaternion::From(double w, double vx, double vy, double vz) {
    Quaternion q;
    q.w  = w;
    q.vx = vx;
    q.vy = vy;
    q.vz = vz;
    return q;
}

Quaternion Quaternion::From(hParam w, hParam vx, hParam vy, hParam vz) {
    Quaternion q;
    q.w  = SK.GetParam(w )->val;
    q.vx = SK.GetParam(vx)->val;
    q.vy = SK.GetParam(vy)->val;
    q.vz = SK.GetParam(vz)->val;
    return q;
}

Quaternion Quaternion::From(Vector axis, double dtheta) {
    Quaternion q;
    double c = cos(dtheta / 2), s = sin(dtheta / 2);
    axis = axis.WithMagnitude(s);
    q.w  = c;
    q.vx = axis.x;
    q.vy = axis.y;
    q.vz = axis.z;
    return q;
}

Quaternion Quaternion::From(Vector u, Vector v)
{
    Vector n = u.Cross(v);

    Quaternion q;
    double s, tr = 1 + u.x + v.y + n.z;
    if(tr > 1e-4) {
        s = 2*sqrt(tr);
        q.w  = s/4;
        q.vx = (v.z - n.y)/s;
        q.vy = (n.x - u.z)/s;
        q.vz = (u.y - v.x)/s;
    } else {
        if(u.x > v.y && u.x > n.z) {
            s = 2*sqrt(1 + u.x - v.y - n.z);
            q.w  = (v.z - n.y)/s;
            q.vx = s/4;
            q.vy = (u.y + v.x)/s;
            q.vz = (n.x + u.z)/s;
        } else if(v.y > n.z) {
            s = 2*sqrt(1 - u.x + v.y - n.z);
            q.w  = (n.x - u.z)/s;
            q.vx = (u.y + v.x)/s;
            q.vy = s/4;
            q.vz = (v.z + n.y)/s;
        } else {
            s = 2*sqrt(1 - u.x - v.y + n.z);
            q.w  = (u.y - v.x)/s;
            q.vx = (n.x + u.z)/s;
            q.vy = (v.z + n.y)/s;
            q.vz = s/4;
        }
    }

    return q.WithMagnitude(1);
}

Quaternion Quaternion::Plus(Quaternion b) const {
    Quaternion q;
    q.w  = w  + b.w;
    q.vx = vx + b.vx;
    q.vy = vy + b.vy;
    q.vz = vz + b.vz;
    return q;
}

Quaternion Quaternion::Minus(Quaternion b) const {
    Quaternion q;
    q.w  = w  - b.w;
    q.vx = vx - b.vx;
    q.vy = vy - b.vy;
    q.vz = vz - b.vz;
    return q;
}

Quaternion Quaternion::ScaledBy(double s) const {
    Quaternion q;
    q.w  = w*s;
    q.vx = vx*s;
    q.vy = vy*s;
    q.vz = vz*s;
    return q;
}

double Quaternion::Magnitude() const {
    return sqrt(w*w + vx*vx + vy*vy + vz*vz);
}

Quaternion Quaternion::WithMagnitude(double s) const {
    return ScaledBy(s/Magnitude());
}

Vector Quaternion::RotationU() const {
    Vector v;
    v.x = w*w + vx*vx - vy*vy - vz*vz;
    v.y = 2*w *vz + 2*vx*vy;
    v.z = 2*vx*vz - 2*w *vy;
    return v;
}

Vector Quaternion::RotationV() const {
    Vector v;
    v.x = 2*vx*vy - 2*w*vz;
    v.y = w*w - vx*vx + vy*vy - vz*vz;
    v.z = 2*w*vx + 2*vy*vz;
    return v;
}

Vector Quaternion::RotationN() const {
    Vector v;
    v.x = 2*w*vy + 2*vx*vz;
    v.y = 2*vy*vz - 2*w*vx;
    v.z = w*w - vx*vx - vy*vy + vz*vz;
    return v;
}

Vector Quaternion::Rotate(Vector p) const {
    // Express the point in the new basis
    return (RotationU().ScaledBy(p.x)).Plus(
            RotationV().ScaledBy(p.y)).Plus(
            RotationN().ScaledBy(p.z));
}

Quaternion Quaternion::Inverse() const {
    Quaternion r;
    r.w = w;
    r.vx = -vx;
    r.vy = -vy;
    r.vz = -vz;
    return r.WithMagnitude(1); // not that the normalize should be reqd
}

Quaternion Quaternion::ToThe(double p) const {
    // Avoid division by zero, or arccos of something not in its domain
    if(w >= (1 - 1e-6)) {
        return From(1, 0, 0, 0);
    } else if(w <= (-1 + 1e-6)) {
        return From(-1, 0, 0, 0);
    }

    Quaternion r;
    Vector axis = Vector::From(vx, vy, vz);
    double theta = acos(w); // okay, since magnitude is 1, so -1 <= w <= 1
    theta *= p;
    r.w = cos(theta);
    axis = axis.WithMagnitude(sin(theta));
    r.vx = axis.x;
    r.vy = axis.y;
    r.vz = axis.z;
    return r;
}

Quaternion Quaternion::Times(Quaternion b) const {
    double sa = w, sb = b.w;
    Vector va = { vx, vy, vz };
    Vector vb = { b.vx, b.vy, b.vz };

    Quaternion r;
    r.w = sa*sb - va.Dot(vb);
    Vector vr = vb.ScaledBy(sa).Plus(
                va.ScaledBy(sb).Plus(
                va.Cross(vb)));
    r.vx = vr.x;
    r.vy = vr.y;
    r.vz = vr.z;
    return r;
}

Quaternion Quaternion::Mirror() const {
    Vector u = RotationU(),
           v = RotationV();
    u = u.ScaledBy(-1);
    v = v.ScaledBy(-1);
    return Quaternion::From(u, v);
}


Vector Vector::From(double x, double y, double z) {
    Vector v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

Vector Vector::From(hParam x, hParam y, hParam z) {
    Vector v;
    v.x = SK.GetParam(x)->val;
    v.y = SK.GetParam(y)->val;
    v.z = SK.GetParam(z)->val;
    return v;
}

double Vector::Element(int i) const {
    switch(i) {
        case 0: return x;
        case 1: return y;
        case 2: return z;
        default: ssassert(false, "Unexpected vector element index");
    }
}

bool Vector::Equals(Vector v, double tol) const {
    // Quick axis-aligned tests before going further
    double dx = v.x - x; if(dx < -tol || dx > tol) return false;
    double dy = v.y - y; if(dy < -tol || dy > tol) return false;
    double dz = v.z - z; if(dz < -tol || dz > tol) return false;

    return (this->Minus(v)).MagSquared() < tol*tol;
}

bool Vector::EqualsExactly(Vector v) const {
    return EXACT(x == v.x &&
                 y == v.y &&
                 z == v.z);
}

Vector Vector::Plus(Vector b) const {
    Vector r;

    r.x = x + b.x;
    r.y = y + b.y;
    r.z = z + b.z;

    return r;
}

Vector Vector::Minus(Vector b) const {
    Vector r;

    r.x = x - b.x;
    r.y = y - b.y;
    r.z = z - b.z;

    return r;
}

Vector Vector::Negated() const {
    Vector r;

    r.x = -x;
    r.y = -y;
    r.z = -z;

    return r;
}

Vector Vector::Cross(Vector b) const {
    Vector r;

    r.x = -(z*b.y) + (y*b.z);
    r.y =  (z*b.x) - (x*b.z);
    r.z = -(y*b.x) + (x*b.y);

    return r;
}

double Vector::Dot(Vector b) const {
    return (x*b.x + y*b.y + z*b.z);
}

double Vector::DirectionCosineWith(Vector b) const {
    Vector a = this->WithMagnitude(1);
    b = b.WithMagnitude(1);
    return a.Dot(b);
}

Vector Vector::Normal(int which) const {
    Vector n;

    // Arbitrarily choose one vector that's normal to us, pivoting
    // appropriately.
    double xa = fabs(x), ya = fabs(y), za = fabs(z);
    if(this->Equals(Vector::From(0, 0, 1))) {
        // Make DXFs exported in the XY plane work nicely...
        n = Vector::From(1, 0, 0);
    } else if(xa < ya && xa < za) {
        n.x = 0;
        n.y = z;
        n.z = -y;
    } else if(ya < za) {
        n.x = -z;
        n.y = 0;
        n.z = x;
    } else {
        n.x = y;
        n.y = -x;
        n.z = 0;
    }

    if(which == 0) {
        // That's the vector we return.
    } else if(which == 1) {
        n = this->Cross(n);
    } else ssassert(false, "Unexpected vector normal index");

    n = n.WithMagnitude(1);

    return n;
}

Vector Vector::RotatedAbout(Vector orig, Vector axis, double theta) const {
    Vector r = this->Minus(orig);
    r = r.RotatedAbout(axis, theta);
    return r.Plus(orig);
}

Vector Vector::RotatedAbout(Vector axis, double theta) const {
    double c = cos(theta);
    double s = sin(theta);

    axis = axis.WithMagnitude(1);

    Vector r;

    r.x =   (x)*(c + (1 - c)*(axis.x)*(axis.x)) +
            (y)*((1 - c)*(axis.x)*(axis.y) - s*(axis.z)) +
            (z)*((1 - c)*(axis.x)*(axis.z) + s*(axis.y));

    r.y =   (x)*((1 - c)*(axis.y)*(axis.x) + s*(axis.z)) +
            (y)*(c + (1 - c)*(axis.y)*(axis.y)) +
            (z)*((1 - c)*(axis.y)*(axis.z) - s*(axis.x));

    r.z =   (x)*((1 - c)*(axis.z)*(axis.x) - s*(axis.y)) +
            (y)*((1 - c)*(axis.z)*(axis.y) + s*(axis.x)) +
            (z)*(c + (1 - c)*(axis.z)*(axis.z));

    return r;
}

Vector Vector::DotInToCsys(Vector u, Vector v, Vector n) const {
    Vector r = {
        this->Dot(u),
        this->Dot(v),
        this->Dot(n)
    };
    return r;
}

Vector Vector::ScaleOutOfCsys(Vector u, Vector v, Vector n) const {
    Vector r = u.ScaledBy(x).Plus(
               v.ScaledBy(y).Plus(
               n.ScaledBy(z)));
    return r;
}

Vector Vector::InPerspective(Vector u, Vector v, Vector n,
                             Vector origin, double cameraTan) const
{
    Vector r = this->Minus(origin);
    r = r.DotInToCsys(u, v, n);
    // yes, minus; we are assuming a csys where u cross v equals n, backwards
    // from the display stuff
    double w = (1 - r.z*cameraTan);
    r = r.ScaledBy(1/w);

    return r;
}

double Vector::DistanceToLine(Vector p0, Vector dp) const {
    double m = dp.Magnitude();
    return ((this->Minus(p0)).Cross(dp)).Magnitude() / m;
}

double Vector::DistanceToPlane(Vector normal, Vector origin) const {
    return this->Dot(normal) - origin.Dot(normal);
}

bool Vector::OnLineSegment(Vector a, Vector b, double tol) const {
    if(this->Equals(a, tol) || this->Equals(b, tol)) return true;

    Vector d = b.Minus(a);

    double m = d.MagSquared();
    double distsq = ((this->Minus(a)).Cross(d)).MagSquared() / m;

    if(distsq >= tol*tol) return false;

    double t = (this->Minus(a)).DivPivoting(d);
    // On-endpoint already tested
    if(t < 0 || t > 1) return false;
    return true;
}

Vector Vector::ClosestPointOnLine(Vector p0, Vector dp) const {
    dp = dp.WithMagnitude(1);
    // this, p0, and (p0+dp) define a plane; the min distance is in
    // that plane, so calculate its normal
    Vector pn = (this->Minus(p0)).Cross(dp);
    // The minimum distance line is in that plane, perpendicular
    // to the line
    Vector n = pn.Cross(dp);

    // Calculate the actual distance
    double d = (dp.Cross(p0.Minus(*this))).Magnitude();
    return this->Plus(n.WithMagnitude(d));
}

double Vector::MagSquared() const {
    return x*x + y*y + z*z;
}

double Vector::Magnitude() const {
    return sqrt(x*x + y*y + z*z);
}

Vector Vector::ScaledBy(double v) const {
    Vector r;

    r.x = x * v;
    r.y = y * v;
    r.z = z * v;

    return r;
}

Vector Vector::WithMagnitude(double v) const {
    double m = Magnitude();
    if(EXACT(m == 0)) {
        // We can do a zero vector with zero magnitude, but not any other cases.
        if(fabs(v) > 1e-100) {
            dbp("Vector::WithMagnitude(%g) of zero vector!", v);
        }
        return From(0, 0, 0);
    } else {
        return ScaledBy(v/m);
    }
}

Vector Vector::ProjectVectorInto(hEntity wrkpl) const {
    EntityBase *w = SK.GetEntity(wrkpl);
    Vector u = w->Normal()->NormalU();
    Vector v = w->Normal()->NormalV();

    double up = this->Dot(u);
    double vp = this->Dot(v);

    return (u.ScaledBy(up)).Plus(v.ScaledBy(vp));
}

Vector Vector::ProjectInto(hEntity wrkpl) const {
    EntityBase *w = SK.GetEntity(wrkpl);
    Vector p0 = w->WorkplaneGetOffset();

    Vector f = this->Minus(p0);

    return p0.Plus(f.ProjectVectorInto(wrkpl));
}

Point2d Vector::Project2d(Vector u, Vector v) const {
    Point2d p;
    p.x = this->Dot(u);
    p.y = this->Dot(v);
    return p;
}

Point2d Vector::ProjectXy() const {
    Point2d p;
    p.x = x;
    p.y = y;
    return p;
}

Vector4 Vector::Project4d() const {
    return Vector4::From(1, x, y, z);
}

double Vector::DivPivoting(Vector delta) const {
    double mx = fabs(delta.x), my = fabs(delta.y), mz = fabs(delta.z);

    if(mx > my && mx > mz) {
        return x/delta.x;
    } else if(my > mz) {
        return y/delta.y;
    } else {
        return z/delta.z;
    }
}

Vector Vector::ClosestOrtho() const {
    double mx = fabs(x), my = fabs(y), mz = fabs(z);

    if(mx > my && mx > mz) {
        return From((x > 0) ? 1 : -1, 0, 0);
    } else if(my > mz) {
        return From(0, (y > 0) ? 1 : -1, 0);
    } else {
        return From(0, 0, (z > 0) ? 1 : -1);
    }
}

Vector Vector::ClampWithin(double minv, double maxv) const {
    Vector ret = *this;

    if(ret.x < minv) ret.x = minv;
    if(ret.y < minv) ret.y = minv;
    if(ret.z < minv) ret.z = minv;

    if(ret.x > maxv) ret.x = maxv;
    if(ret.y > maxv) ret.y = maxv;
    if(ret.z > maxv) ret.z = maxv;

    return ret;
}

void Vector::MakeMaxMin(Vector *maxv, Vector *minv) const {
    maxv->x = max(maxv->x, x);
    maxv->y = max(maxv->y, y);
    maxv->z = max(maxv->z, z);

    minv->x = min(minv->x, x);
    minv->y = min(minv->y, y);
    minv->z = min(minv->z, z);
}

bool Vector::OutsideAndNotOn(Vector maxv, Vector minv) const {
    return (x > maxv.x + LENGTH_EPS) || (x < minv.x - LENGTH_EPS) ||
           (y > maxv.y + LENGTH_EPS) || (y < minv.y - LENGTH_EPS) ||
           (z > maxv.z + LENGTH_EPS) || (z < minv.z - LENGTH_EPS);
}

bool Vector::BoundingBoxesDisjoint(Vector amax, Vector amin,
                                   Vector bmax, Vector bmin)
{
    int i;
    for(i = 0; i < 3; i++) {
        if(amax.Element(i) < bmin.Element(i) - LENGTH_EPS) return true;
        if(amin.Element(i) > bmax.Element(i) + LENGTH_EPS) return true;
    }
    return false;
}

#if FULL_LIB_JJS
bool Vector::BoundingBoxIntersectsLine(Vector amax, Vector amin,
                                       Vector p0, Vector p1, bool asSegment)
{
    Vector dp = p1.Minus(p0);
    double lp = dp.Magnitude();
    dp = dp.ScaledBy(1.0/lp);

    int i, a;
    for(i = 0; i < 3; i++) {
        int j = WRAP(i+1, 3), k = WRAP(i+2, 3);
        if(lp*fabs(dp.Element(i)) < LENGTH_EPS) continue; // parallel to plane

        for(a = 0; a < 2; a++) {
            double d = (a == 0) ? amax.Element(i) : amin.Element(i);
            // n dot (p0 + t*dp) = d
            // (n dot p0) + t * (n dot dp) = d
            double t = (d - p0.Element(i)) / dp.Element(i);
            Vector p = p0.Plus(dp.ScaledBy(t));

            if(asSegment && (t < -LENGTH_EPS || t > (lp+LENGTH_EPS))) continue;

            if(p.Element(j) > amax.Element(j) + LENGTH_EPS) continue;
            if(p.Element(k) > amax.Element(k) + LENGTH_EPS) continue;

            if(p.Element(j) < amin.Element(j) - LENGTH_EPS) continue;
            if(p.Element(k) < amin.Element(k) - LENGTH_EPS) continue;

            return true;
        }
    }

    return false;
}
#endif

Vector Vector::AtIntersectionOfPlanes(Vector n1, double d1,
                                      Vector n2, double d2)
{
    double det = (n1.Dot(n1))*(n2.Dot(n2)) -
                 (n1.Dot(n2))*(n1.Dot(n2));
    double c1 = (d1*n2.Dot(n2) - d2*n1.Dot(n2))/det;
    double c2 = (d2*n1.Dot(n1) - d1*n1.Dot(n2))/det;

    return (n1.ScaledBy(c1)).Plus(n2.ScaledBy(c2));
}

void Vector::ClosestPointBetweenLines(Vector a0, Vector da,
                                      Vector b0, Vector db,
                                      double *ta, double *tb)
{
    // Make a semi-orthogonal coordinate system from those directions;
    // note that dna and dnb need not be perpendicular.
    Vector dn = da.Cross(db); // normal to both
    Vector dna = dn.Cross(da); // normal to da
    Vector dnb = dn.Cross(db); // normal to db

    // At the intersection of the lines
    //    a0 + pa*da = b0 + pb*db (where pa, pb are scalar params)
    // So dot this equation against dna and dnb to get two equations
    // to solve for da and db
    *tb =  ((a0.Minus(b0)).Dot(dna))/(db.Dot(dna));
    *ta = -((a0.Minus(b0)).Dot(dnb))/(da.Dot(dnb));
}

Vector Vector::AtIntersectionOfLines(Vector a0, Vector a1,
                                     Vector b0, Vector b1,
                                     bool *skew,
                                     double *parama, double *paramb)
{
    Vector da = a1.Minus(a0), db = b1.Minus(b0);

    double pa, pb;
    Vector::ClosestPointBetweenLines(a0, da, b0, db, &pa, &pb);

    if(parama) *parama = pa;
    if(paramb) *paramb = pb;

    // And from either of those, we get the intersection point.
    Vector pi = a0.Plus(da.ScaledBy(pa));

    if(skew) {
        // Check if the intersection points on each line are actually
        // coincident...
        if(pi.Equals(b0.Plus(db.ScaledBy(pb)))) {
            *skew = false;
        } else {
            *skew = true;
        }
    }
    return pi;
}

Vector Vector::AtIntersectionOfPlaneAndLine(Vector n, double d,
                                            Vector p0, Vector p1,
                                            bool *parallel)
{
    Vector dp = p1.Minus(p0);

    if(fabs(n.Dot(dp)) < LENGTH_EPS) {
        if(parallel) *parallel = true;
        return Vector::From(0, 0, 0);
    }

    if(parallel) *parallel = false;

    // n dot (p0 + t*dp) = d
    // (n dot p0) + t * (n dot dp) = d
    double t = (d - n.Dot(p0)) / (n.Dot(dp));

    return p0.Plus(dp.ScaledBy(t));
}

static double det2(double a1, double b1,
                   double a2, double b2)
{
    return (a1*b2) - (b1*a2);
}
static double det3(double a1, double b1, double c1,
                   double a2, double b2, double c2,
                   double a3, double b3, double c3)
{
    return a1*det2(b2, c2, b3, c3) -
           b1*det2(a2, c2, a3, c3) +
           c1*det2(a2, b2, a3, b3);
}
Vector Vector::AtIntersectionOfPlanes(Vector na, double da,
                                      Vector nb, double db,
                                      Vector nc, double dc,
                                      bool *parallel)
{
    double det  = det3(na.x, na.y, na.z,
                       nb.x, nb.y, nb.z,
                       nc.x, nc.y, nc.z);
    if(fabs(det) < 1e-10) { // arbitrary tolerance, not so good
        *parallel = true;
        return Vector::From(0, 0, 0);
    }
    *parallel = false;

    double detx = det3(da,   na.y, na.z,
                       db,   nb.y, nb.z,
                       dc,   nc.y, nc.z);

    double dety = det3(na.x, da,   na.z,
                       nb.x, db,   nb.z,
                       nc.x, dc,   nc.z);

    double detz = det3(na.x, na.y, da,
                       nb.x, nb.y, db,
                       nc.x, nc.y, dc  );

    return Vector::From(detx/det, dety/det, detz/det);
}

size_t VectorHash::operator()(const Vector &v) const {
    const size_t size = (size_t)pow(std::numeric_limits<size_t>::max(), 1.0 / 3.0) - 1;
    const double eps = 4.0 * LENGTH_EPS;

    double x = fabs(v.x) / eps;
    double y = fabs(v.y) / eps;
    double z = fabs(v.y) / eps;

    size_t xs = size_t(fmod(x, (double)size));
    size_t ys = size_t(fmod(y, (double)size));
    size_t zs = size_t(fmod(z, (double)size));

    return (zs * size + ys) * size + xs;
}

bool VectorPred::operator()(Vector a, Vector b) const {
    return a.Equals(b, LENGTH_EPS);
}

Vector4 Vector4::From(double w, double x, double y, double z) {
    Vector4 ret;
    ret.w = w;
    ret.x = x;
    ret.y = y;
    ret.z = z;
    return ret;
}

Vector4 Vector4::From(double w, Vector v) {
    return Vector4::From(w, w*v.x, w*v.y, w*v.z);
}

Vector4 Vector4::Blend(Vector4 a, Vector4 b, double t) {
    return (a.ScaledBy(1 - t)).Plus(b.ScaledBy(t));
}

Vector4 Vector4::Plus(Vector4 b) const {
    return Vector4::From(w + b.w, x + b.x, y + b.y, z + b.z);
}

Vector4 Vector4::Minus(Vector4 b) const {
    return Vector4::From(w - b.w, x - b.x, y - b.y, z - b.z);
}

Vector4 Vector4::ScaledBy(double s) const {
    return Vector4::From(w*s, x*s, y*s, z*s);
}

Vector Vector4::PerspectiveProject() const {
    return Vector::From(x / w, y / w, z / w);
}

Point2d Point2d::From(double x, double y) {
    return { x, y };
}

Point2d Point2d::FromPolar(double r, double a) {
    return { r * cos(a), r * sin(a) };
}

double Point2d::Angle() const {
    double a = atan2(y, x);
    return M_PI + remainder(a - M_PI, 2 * M_PI);
}

double Point2d::AngleTo(const Point2d &p) const {
    return p.Minus(*this).Angle();
}

Point2d Point2d::Plus(const Point2d &b) const {
    return { x + b.x, y + b.y };
}

Point2d Point2d::Minus(const Point2d &b) const {
    return { x - b.x, y - b.y };
}

Point2d Point2d::ScaledBy(double s) const {
    return { x * s, y * s };
}

double Point2d::DivPivoting(Point2d delta) const {
    if(fabs(delta.x) > fabs(delta.y)) {
        return x/delta.x;
    } else {
        return y/delta.y;
    }
}

double Point2d::MagSquared() const {
    return x*x + y*y;
}

double Point2d::Magnitude() const {
    return sqrt(x*x + y*y);
}

Point2d Point2d::WithMagnitude(double v) const {
    double m = Magnitude();
    if(m < 1e-20) {
        dbp("!!! WithMagnitude() of zero vector");
        return { v, 0 };
    }
    return { x * v / m, y * v / m };
}

double Point2d::DistanceTo(const Point2d &p) const {
    double dx = x - p.x;
    double dy = y - p.y;
    return sqrt(dx*dx + dy*dy);
}

double Point2d::Dot(Point2d p) const {
    return x*p.x + y*p.y;
}

double Point2d::DistanceToLine(const Point2d &p0, const Point2d &dp, bool asSegment) const {
    double m = dp.x*dp.x + dp.y*dp.y;
    if(m < LENGTH_EPS*LENGTH_EPS) return VERY_POSITIVE;

    // Let our line be p = p0 + t*dp, for a scalar t from 0 to 1
    double t = (dp.x*(x - p0.x) + dp.y*(y - p0.y))/m;

    if(asSegment) {
        if(t < 0.0) return DistanceTo(p0);
        if(t > 1.0) return DistanceTo(p0.Plus(dp));
    }
    Point2d closest = p0.Plus(dp.ScaledBy(t));
    return DistanceTo(closest);
}

double Point2d::DistanceToLineSigned(const Point2d &p0, const Point2d &dp, bool asSegment) const {
    double m = dp.x*dp.x + dp.y*dp.y;
    if(m < LENGTH_EPS*LENGTH_EPS) return VERY_POSITIVE;

    Point2d n = dp.Normal().WithMagnitude(1.0);
    double dist = n.Dot(*this) - n.Dot(p0);
    if(asSegment) {
        // Let our line be p = p0 + t*dp, for a scalar t from 0 to 1
        double t = (dp.x*(x - p0.x) + dp.y*(y - p0.y))/m;
        double sign = (dist > 0.0) ? 1.0 : -1.0;
        if(t < 0.0) return DistanceTo(p0) * sign;
        if(t > 1.0) return DistanceTo(p0.Plus(dp)) * sign;
    }

    return dist;
}

Point2d Point2d::Normal() const {
    return { y, -x };
}

bool Point2d::Equals(Point2d v, double tol) const {
    double dx = v.x - x; if(dx < -tol || dx > tol) return false;
    double dy = v.y - y; if(dy < -tol || dy > tol) return false;

    return (this->Minus(v)).MagSquared() < tol*tol;
}

BBox BBox::From(const Vector &p0, const Vector &p1) {
    BBox bbox;
    bbox.minp.x = min(p0.x, p1.x);
    bbox.minp.y = min(p0.y, p1.y);
    bbox.minp.z = min(p0.z, p1.z);

    bbox.maxp.x = max(p0.x, p1.x);
    bbox.maxp.y = max(p0.y, p1.y);
    bbox.maxp.z = max(p0.z, p1.z);
    return bbox;
}

Vector BBox::GetOrigin() const { return minp.Plus(maxp.Minus(minp).ScaledBy(0.5)); }
Vector BBox::GetExtents() const { return maxp.Minus(minp).ScaledBy(0.5); }

void BBox::Include(const Vector &v, double r) {
    minp.x = min(minp.x, v.x - r);
    minp.y = min(minp.y, v.y - r);
    minp.z = min(minp.z, v.z - r);

    maxp.x = max(maxp.x, v.x + r);
    maxp.y = max(maxp.y, v.y + r);
    maxp.z = max(maxp.z, v.z + r);
}

bool BBox::Overlaps(const BBox &b1) const {
    Vector t = b1.GetOrigin().Minus(GetOrigin());
    Vector e = b1.GetExtents().Plus(GetExtents());

    return fabs(t.x) < e.x && fabs(t.y) < e.y && fabs(t.z) < e.z;
}

bool BBox::Contains(const Point2d &p, double r) const {
    return p.x >= (minp.x - r) &&
           p.y >= (minp.y - r) &&
           p.x <= (maxp.x + r) &&
           p.y <= (maxp.y + r);
}

const std::vector<double>& SolveSpace::StipplePatternDashes(StipplePattern pattern) {
    static bool initialized;
    static std::vector<double> dashes[(size_t)StipplePattern::LAST + 1];
    if(!initialized) {
        // Inkscape ignores all elements that are exactly zero instead of drawing
        // them as dots, so set those to 1e-6.
        dashes[(size_t)StipplePattern::CONTINUOUS] =
            {};
        dashes[(size_t)StipplePattern::SHORT_DASH] =
            { 1.0, 2.0 };
        dashes[(size_t)StipplePattern::DASH] =
            { 1.0, 1.0 };
        dashes[(size_t)StipplePattern::DASH_DOT] =
            { 1.0, 0.5, 1e-6, 0.5 };
        dashes[(size_t)StipplePattern::DASH_DOT_DOT] =
            { 1.0, 0.5, 1e-6, 0.5, 1e-6, 0.5 };
        dashes[(size_t)StipplePattern::DOT] =
            { 1e-6, 0.5 };
        dashes[(size_t)StipplePattern::LONG_DASH] =
            { 2.0, 0.5 };
        dashes[(size_t)StipplePattern::FREEHAND] =
            { 1.0, 2.0 };
        dashes[(size_t)StipplePattern::ZIGZAG] =
            { 1.0, 2.0 };
    }

    return dashes[(size_t)pattern];
}

double SolveSpace::StipplePatternLength(StipplePattern pattern) {
    static bool initialized;
    static double lengths[(size_t)StipplePattern::LAST + 1];
    if(!initialized) {
        for(size_t i = 0; i < (size_t)StipplePattern::LAST; i++) {
            const std::vector<double> &dashes = StipplePatternDashes((StipplePattern)i);
            double length = 0.0;
            for(double dash : dashes) {
                length += dash;
            }
            lengths[i] = length;
        }
    }

    return lengths[(size_t)pattern];
}
