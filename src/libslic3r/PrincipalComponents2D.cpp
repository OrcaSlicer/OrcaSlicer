#include "PrincipalComponents2D.hpp"
#include "Point.hpp"

namespace Slic3r {



// returns triangle area, first_moment_of_area_xy, second_moment_of_area_xy, second_moment_of_area_covariance
// none of the values is divided/normalized by area.
// The function computes intgeral over the area of the triangle, with function f(x,y) = x for first moments of area (y is analogous)
// f(x,y) = x^2 for second moment of area
// and f(x,y) = x*y for second moment of area covariance
std::tuple<float, Vec2f, Vec2f, float> compute_moments_of_area_of_triangle(const Vec2f &a, const Vec2f &b, const Vec2f &c)
{
    // based on the following guide:
    // Denote the vertices of S by a, b, c. Then the map
    //  g:(u,v)↦a+u(b−a)+v(c−a) ,
    //  which in coordinates appears as
    //  g:(u,v)↦{x(u,v)y(u,v)=a1+u(b1−a1)+v(c1−a1)=a2+u(b2−a2)+v(c2−a2) ,(1)
    //  obviously maps S′ bijectively onto S. Therefore the transformation formula for multiple integrals steps into action, and we obtain
    //  ∫Sf(x,y)d(x,y)=∫S′f(x(u,v),y(u,v))∣∣Jg(u,v)∣∣ d(u,v) .
    //  In the case at hand the Jacobian determinant is a constant: From (1) we obtain
    //  Jg(u,v)=det[xuyuxvyv]=(b1−a1)(c2−a2)−(c1−a1)(b2−a2) .
    //  Therefore we can write
    //  ∫Sf(x,y)d(x,y)=∣∣Jg∣∣∫10∫1−u0f~(u,v) dv du ,
    //  where f~ denotes the pullback of f to S′:
    //  f~(u,v):=f(x(u,v),y(u,v)) .
    //  Don't forget taking the absolute value of Jg!

    float jacobian_determinant_abs = std::abs((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y()));

    // coordinate transform: gx(u,v) = a.x + u * (b.x - a.x) + v * (c.x - a.x)
    // coordinate transform: gy(u,v) = a.y + u * (b.y - a.y) + v * (c.y - a.y)
    // second moment of area for x: f(x, y) = x^2;
    //              f(gx(u,v), gy(u,v)) = gx(u,v)^2 = ... (long expanded form)

    // result is Int_T func = jacobian_determinant_abs * Int_0^1 Int_0^1-u func(gx(u,v), gy(u,v)) dv du
    // integral_0^1 integral_0^(1 - u) (a + u (b - a) + v (c - a))^2 dv du = 1/12 (a^2 + a (b + c) + b^2 + b c + c^2)

    Vec2f second_moment_of_area_xy = jacobian_determinant_abs *
                                     (a.cwiseProduct(a) + b.cwiseProduct(b) + b.cwiseProduct(c) + c.cwiseProduct(c) +
                                      a.cwiseProduct(b + c)) /
                                     12.0f;
    // second moment of area covariance : f(x, y) = x*y;
    //              f(gx(u,v), gy(u,v)) = gx(u,v)*gy(u,v) = ... (long expanded form)
    //(a_1 + u * (b_1 - a_1) + v * (c_1 - a_1)) * (a_2 + u * (b_2 - a_2) + v * (c_2 - a_2))
    // ==    (a_1 + u (b_1 - a_1) + v (c_1 - a_1)) (a_2 + u (b_2 - a_2) + v (c_2 - a_2))

    // intermediate result: integral_0^(1 - u) (a_1 + u (b_1 - a_1) + v (c_1 - a_1)) (a_2 + u (b_2 - a_2) + v (c_2 - a_2)) dv =
    //  1/6 (u - 1) (-c_1 (u - 1) (a_2 (u - 1) - 3 b_2 u) - c_2 (u - 1) (a_1 (u - 1) - 3 b_1 u + 2 c_1 (u - 1)) + 3 b_1 u (a_2 (u - 1) - 2
    //  b_2 u) + a_1 (u - 1) (3 b_2 u - 2 a_2 (u - 1))) result = integral_0^1 1/6 (u - 1) (-c_1 (u - 1) (a_2 (u - 1) - 3 b_2 u) - c_2 (u -
    //  1) (a_1 (u - 1) - 3 b_1 u + 2 c_1 (u - 1)) + 3 b_1 u (a_2 (u - 1) - 2 b_2 u) + a_1 (u - 1) (3 b_2 u - 2 a_2 (u - 1))) du =
    //   1/24 (a_2 (b_1 + c_1) + a_1 (2 a_2 + b_2 + c_2) + b_2 c_1 + b_1 c_2 + 2 b_1 b_2 + 2 c_1 c_2)
    //  result is Int_T func = jacobian_determinant_abs * Int_0^1 Int_0^1-u func(gx(u,v), gy(u,v)) dv du
    float second_moment_of_area_covariance = jacobian_determinant_abs * (1.0f / 24.0f) *
                                             (a.y() * (b.x() + c.x()) + a.x() * (2.0f * a.y() + b.y() + c.y()) + b.y() * c.x() +
                                              b.x() * c.y() + 2.0f * b.x() * b.y() + 2.0f * c.x() * c.y());

    float area = jacobian_determinant_abs * 0.5f;

    Vec2f first_moment_of_area_xy = jacobian_determinant_abs * (a + b + c) / 6.0f;

    return {area, first_moment_of_area_xy, second_moment_of_area_xy, second_moment_of_area_covariance};
};

// returns two eigenvectors of the area covered by given polygons. The vectors are sorted by their corresponding eigenvalue, largest first
std::tuple<Vec2f, Vec2f> compute_principal_components(const Polygons &polys)
{
    Vec2f centroid_accumulator                         = Vec2f::Zero();
    Vec2f second_moment_of_area_accumulator            = Vec2f::Zero();
    float second_moment_of_area_covariance_accumulator = 0.0f;
    float area                                         = 0.0f;

    for (const Polygon &poly : polys) {
        Vec2f p0 = unscaled(poly.first_point()).cast<float>();
        for (size_t i = 2; i < poly.points.size(); i++) {
            Vec2f p1 = unscaled(poly.points[i - 1]).cast<float>();
            Vec2f p2 = unscaled(poly.points[i]).cast<float>();

            float sign = cross2(p1 - p0, p2 - p1) > 0 ? 1.0f : -1.0f;

            auto [triangle_area, first_moment_of_area, second_moment_area,
                  second_moment_of_area_covariance] = compute_moments_of_area_of_triangle(p0, p1, p2);
            area += sign * triangle_area;
            centroid_accumulator += sign * first_moment_of_area;
            second_moment_of_area_accumulator += sign * second_moment_area;
            second_moment_of_area_covariance_accumulator += sign * second_moment_of_area_covariance;
        }
    }

    if (area <= 0.0) {
        return {Vec2f::Zero(), Vec2f::Zero()};
    }

    Vec2f  centroid   = centroid_accumulator / area;
    Vec2f  variance   = second_moment_of_area_accumulator / area - centroid.cwiseProduct(centroid);
    double covariance = second_moment_of_area_covariance_accumulator / area - centroid.x() * centroid.y();
#if 0
        std::cout << "area : " << area << std::endl;
        std::cout << "variancex : " << variance.x() << std::endl;
        std::cout << "variancey : " << variance.y() << std::endl;
        std::cout << "covariance : " << covariance << std::endl;
#endif
    if (abs(covariance) < EPSILON) {
        std::tuple<Vec2f, Vec2f> result{Vec2f{variance.x(), 0.0}, Vec2f{0.0, variance.y()}};
        if (variance.y() > variance.x()) {
            return {std::get<1>(result), std::get<0>(result)};
        } else
            return result;
    }

    // now we find the first principal component of the covered area by computing max eigenvalue and the correspoding eigenvector of
    // covariance matrix
    //  covaraince matrix C is :  | VarX  Cov  |
    //                            | Cov   VarY |
    // Eigenvalues are solutions to det(C - lI) = 0, where l is the eigenvalue and I unit matrix
    // Eigenvector for eigenvalue l is any vector v such that Cv = lv

    float eigenvalue_a = 0.5f * (variance.x() + variance.y() +
                                 sqrt((variance.x() - variance.y()) * (variance.x() - variance.y()) + 4.0f * covariance * covariance));
    float eigenvalue_b = 0.5f * (variance.x() + variance.y() -
                                 sqrt((variance.x() - variance.y()) * (variance.x() - variance.y()) + 4.0f * covariance * covariance));
    Vec2f  eigenvector_a{(eigenvalue_a - variance.y()) / covariance, 1.0f};
    Vec2f  eigenvector_b{(eigenvalue_b - variance.y()) / covariance, 1.0f};

#if 0
        std::cout << "eigenvalue_a: " << eigenvalue_a << std::endl;
        std::cout << "eigenvalue_b: " << eigenvalue_b << std::endl;
        std::cout << "eigenvectorA: " << eigenvector_a.x() <<  "  " << eigenvector_a.y() << std::endl;
        std::cout << "eigenvectorB: " << eigenvector_b.x() <<  "  " << eigenvector_b.y() << std::endl;
#endif

    if (eigenvalue_a > eigenvalue_b) {
        return {eigenvector_a, eigenvector_b};
    } else {
        return {eigenvector_b, eigenvector_a};
    }
}

}