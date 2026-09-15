#include "libslic3r/CAD/SketchConstraints.hpp"
#include <Eigen/Dense>
#include <cmath>

namespace Slic3r {

int SketchConstraints::add_point(double x, double y)
{
    m_vars.push_back(x);
    m_vars.push_back(y);
    return static_cast<int>(m_vars.size() / 2) - 1;
}

void SketchConstraints::set_point(int id, double x, double y)
{
    size_t idx = 2 * id;
    m_vars[idx]     = x;
    m_vars[idx + 1] = y;
}

Vec2d SketchConstraints::get_point(int id) const
{
    size_t idx = 2 * id;
    return Vec2d(m_vars[idx], m_vars[idx + 1]);
}

int SketchConstraints::point_count() const
{
    return static_cast<int>(m_vars.size() / 2);
}

void SketchConstraints::fix_point(int id)
{
    size_t idx = 2 * id;
    Con c;
    c.type = FIX_POINT;
    c.a    = id;
    c.b = c.c = c.d = 0;
    c.k0   = m_vars[idx];
    c.k1   = m_vars[idx + 1];
    m_cons.push_back(c);
}

void SketchConstraints::coincident(int a, int b)
{
    Con c;
    c.type = COINCIDENT;
    c.a = a; c.b = b; c.c = c.d = 0;
    c.k0 = c.k1 = 0;
    m_cons.push_back(c);
}

void SketchConstraints::horizontal(int a, int b)
{
    Con c;
    c.type = HORIZONTAL;
    c.a = a; c.b = b; c.c = c.d = 0;
    c.k0 = c.k1 = 0;
    m_cons.push_back(c);
}

void SketchConstraints::vertical(int a, int b)
{
    Con c;
    c.type = VERTICAL;
    c.a = a; c.b = b; c.c = c.d = 0;
    c.k0 = c.k1 = 0;
    m_cons.push_back(c);
}

void SketchConstraints::distance(int a, int b, double d)
{
    Con c;
    c.type = DISTANCE;
    c.a = a; c.b = b; c.c = c.d = 0;
    c.k0 = d; c.k1 = 0;
    m_cons.push_back(c);
}

void SketchConstraints::lock_x(int id, double x)
{
    Con c;
    c.type = LOCK_X;
    c.a = id;
    c.b = c.c = c.d = 0;
    c.k0 = x; c.k1 = 0;
    m_cons.push_back(c);
}

void SketchConstraints::lock_y(int id, double y)
{
    Con c;
    c.type = LOCK_Y;
    c.a = id;
    c.b = c.c = c.d = 0;
    c.k0 = y; c.k1 = 0;
    m_cons.push_back(c);
}

void SketchConstraints::equal_length(int a, int b, int c, int d)
{
    Con con;
    con.type = EQUAL_LENGTH;
    con.a = a; con.b = b; con.c = c; con.d = d;
    con.k0 = con.k1 = 0;
    m_cons.push_back(con);
}

void SketchConstraints::parallel(int a, int b, int c, int d)
{
    Con con;
    con.type = PARALLEL;
    con.a = a; con.b = b; con.c = c; con.d = d;
    con.k0 = con.k1 = 0;
    m_cons.push_back(con);
}

void SketchConstraints::perpendicular(int a, int b, int c, int d)
{
    Con con;
    con.type = PERPENDICULAR;
    con.a = a; con.b = b; con.c = c; con.d = d;
    con.k0 = con.k1 = 0;
    m_cons.push_back(con);
}

void SketchConstraints::midpoint(int m, int a, int b)
{
    Con con;
    con.type = MIDPOINT;
    con.a = m; con.b = a; con.c = b; con.d = -1;
    con.k0 = con.k1 = 0;
    m_cons.push_back(con);
}

void SketchConstraints::symmetric(int a, int b, int c, int d)
{
    Con con;
    con.type = SYMMETRIC;
    con.a = a; con.b = b; con.c = c; con.d = d;
    con.k0 = con.k1 = 0;
    m_cons.push_back(con);
}

void SketchConstraints::angle(int a, int b, int c, int d, double radians)
{
    Con con;
    con.type = ANGLE;
    con.a = a; con.b = b; con.c = c; con.d = d;
    con.k0 = radians; con.k1 = 0;
    m_cons.push_back(con);
}

void SketchConstraints::point_line_distance(int p, int a, int b, double dist)
{
    Con con;
    con.type = PT_LINE_DIST;
    con.a = p; con.b = a; con.c = b; con.d = -1;
    con.k0 = dist; con.k1 = 0;
    m_cons.push_back(con);
}

Eigen::VectorXd SketchConstraints::residuals(const std::vector<double>& v) const
{
    auto X = [&](int i) { return v[2 * i]; };
    auto Y = [&](int i) { return v[2 * i + 1]; };

    std::vector<double> res;
    for (const auto& c : m_cons) {
        switch (c.type) {
        case FIX_POINT:
            res.push_back(X(c.a) - c.k0);
            res.push_back(Y(c.a) - c.k1);
            break;
        case COINCIDENT:
            res.push_back(X(c.a) - X(c.b));
            res.push_back(Y(c.a) - Y(c.b));
            break;
        case HORIZONTAL:
            res.push_back(Y(c.a) - Y(c.b));
            break;
        case VERTICAL:
            res.push_back(X(c.a) - X(c.b));
            break;
        case DISTANCE:
            res.push_back(std::hypot(X(c.a) - X(c.b), Y(c.a) - Y(c.b)) - c.k0);
            break;
        case LOCK_X:
            res.push_back(X(c.a) - c.k0);
            break;
        case LOCK_Y:
            res.push_back(Y(c.a) - c.k0);
            break;
        case EQUAL_LENGTH:
            res.push_back(std::hypot(X(c.a) - X(c.b), Y(c.a) - Y(c.b)) -
                          std::hypot(X(c.c) - X(c.d), Y(c.c) - Y(c.d)));
            break;
        case PARALLEL:
            res.push_back((X(c.b) - X(c.a)) * (Y(c.d) - Y(c.c)) -
                          (Y(c.b) - Y(c.a)) * (X(c.d) - X(c.c)));
            break;
        case PERPENDICULAR:
            res.push_back((X(c.b) - X(c.a)) * (X(c.d) - X(c.c)) +
                          (Y(c.b) - Y(c.a)) * (Y(c.d) - Y(c.c)));
            break;
        case MIDPOINT:
            res.push_back(X(c.a) - 0.5 * (X(c.b) + X(c.c)));
            res.push_back(Y(c.a) - 0.5 * (Y(c.b) + Y(c.c)));
            break;
        case SYMMETRIC: {
            const double abx = X(c.b) - X(c.a), aby = Y(c.b) - Y(c.a);
            const double cdx = X(c.d) - X(c.c), cdy = Y(c.d) - Y(c.c);
            res.push_back(abx * cdx + aby * cdy);
            const double mx = 0.5 * (X(c.a) + X(c.b));
            const double my = 0.5 * (Y(c.a) + Y(c.b));
            res.push_back((mx - X(c.c)) * cdy - (my - Y(c.c)) * cdx);
            break;
        }
        case ANGLE: {
            const double ux = X(c.b) - X(c.a), uy = Y(c.b) - Y(c.a);
            const double wx = X(c.d) - X(c.c), wy = Y(c.d) - Y(c.c);
            const double cross = ux * wy - uy * wx;
            const double dot   = ux * wx + uy * wy;
            res.push_back(std::atan2(cross, dot) - c.k0);
            break;
        }
        case PT_LINE_DIST: {
            const double bx = X(c.b), by = Y(c.b);
            const double cx = X(c.c), cy = Y(c.c);
            const double L  = std::hypot(cx - bx, cy - by);
            const double num = (X(c.a) - bx) * (cy - by) - (Y(c.a) - by) * (cx - bx);
            res.push_back((L > 1e-12 ? std::abs(num) / L : 0.0) - c.k0);
            break;
        }
        }
    }

    Eigen::VectorXd r(static_cast<Eigen::Index>(res.size()));
    for (size_t i = 0; i < res.size(); ++i)
        r(static_cast<Eigen::Index>(i)) = res[i];
    return r;
}

Eigen::MatrixXd SketchConstraints::jacobian(const std::vector<double>& v) const
{
    int m = static_cast<int>(residuals(v).size());
    int n = static_cast<int>(v.size());
    Eigen::MatrixXd J(m, n);
    const double eps = 1e-7;

    std::vector<double> vp = v;
    std::vector<double> vm = v;

    for (int j = 0; j < n; ++j) {
        vp[j] = v[j] + eps;
        vm[j] = v[j] - eps;
        Eigen::VectorXd rp = residuals(vp);
        Eigen::VectorXd rm = residuals(vm);
        vp[j] = v[j];
        vm[j] = v[j];
        J.col(j) = (rp - rm) / (2.0 * eps);
    }

    return J;
}

bool SketchConstraints::solve(int max_iter, double tol)
{
    if (m_cons.empty()) return true;
    double lambda = 1e-3;
    Eigen::VectorXd r = residuals(m_vars);
    for (int it = 0; it < max_iter; ++it) {
        double rn = r.norm();
        if (rn < tol) return true;
        Eigen::MatrixXd J = jacobian(m_vars);
        Eigen::MatrixXd A = J.transpose() * J;
        Eigen::VectorXd g = J.transpose() * r;
        bool stepped = false;
        for (int t = 0; t < 12; ++t) {
            Eigen::MatrixXd Ad = A;
            for (int i = 0; i < Ad.rows(); ++i)
                Ad(i, i) += lambda * (1.0 + Ad(i, i));
            Eigen::VectorXd dx = Ad.ldlt().solve(-g);
            std::vector<double> cand = m_vars;
            for (size_t i = 0; i < cand.size(); ++i)
                cand[i] += dx[static_cast<Eigen::Index>(i)];
            Eigen::VectorXd rc = residuals(cand);
            if (rc.norm() < rn) {
                m_vars = cand;
                r      = rc;
                lambda = std::max(lambda * 0.4, 1e-12);
                stepped = true;
                break;
            }
            lambda *= 3.0;
        }
        if (!stepped) break;
    }
    return r.norm() < tol * 100;
}

double SketchConstraints::residual_norm() const
{
    return residuals(m_vars).norm();
}

} // namespace Slic3r
