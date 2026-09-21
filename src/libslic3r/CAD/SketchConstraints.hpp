#ifndef slic3r_SketchConstraints_hpp_
#define slic3r_SketchConstraints_hpp_

#include "libslic3r/Point.hpp"
#include <vector>
#include <Eigen/Dense>

namespace Slic3r {

class SketchConstraints {
public:
    int   add_point(double x, double y);
    void  set_point(int id, double x, double y);
    Vec2d get_point(int id) const;
    int   point_count() const;

    void fix_point(int id);
    void coincident(int a, int b);
    void horizontal(int a, int b);
    void vertical(int a, int b);
    void distance(int a, int b, double d);
    void lock_x(int id, double x);
    void lock_y(int id, double y);
    void equal_length(int a, int b, int c, int d);
    void parallel(int a, int b, int c, int d);
    void perpendicular(int a, int b, int c, int d);
    void midpoint(int m, int a, int b);
    void symmetric(int a, int b, int c, int d);
    void angle(int a, int b, int c, int d, double radians);
    void point_line_distance(int p, int a, int b, double dist);

    bool   solve(int max_iter = 200, double tol = 1e-10);
    double residual_norm() const;

private:
    std::vector<double> m_vars;

    enum ConType : int {
        FIX_POINT = 0,
        COINCIDENT,
        HORIZONTAL,
        VERTICAL,
        DISTANCE,
        LOCK_X,
        LOCK_Y,
        EQUAL_LENGTH,
        PARALLEL,
        PERPENDICULAR,
        MIDPOINT,
        SYMMETRIC,
        ANGLE,
        PT_LINE_DIST
    };

    struct Con {
        int    type;
        int    a, b, c, d;
        double k0, k1;
    };
    std::vector<Con> m_cons;

    Eigen::VectorXd residuals(const std::vector<double>& v) const;
    Eigen::MatrixXd jacobian(const std::vector<double>& v) const;
};

} // namespace Slic3r

#endif // slic3r_SketchConstraints_hpp_
