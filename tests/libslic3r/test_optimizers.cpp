#include <catch2/catch_all.hpp>
#include "test_utils.hpp"

#include <libslic3r/Optimize/BruteforceOptimizer.hpp>

#include <libslic3r/Optimize/NLoptOptimizer.hpp>

void check_opt_result(double score, double ref, double abs_err, double rel_err)
{
    double abs_diff = std::abs(score - ref);
    double rel_diff = std::abs(abs_diff / std::abs(ref));

    bool abs_reached = abs_diff < abs_err;
    bool rel_reached = rel_diff < rel_err;
    bool precision_reached = abs_reached || rel_reached;
    REQUIRE(precision_reached);
}

template<class Opt> void test_sin(Opt &&opt)
{
    using namespace Slic3r::opt;

    auto optfunc = [](const auto &in) {
        auto [phi] = in;

        return std::sin(phi);
    };

    auto init = initvals({PI});
    auto optbounds = bounds({ {0., 2 * PI}});

    Result result_min = opt.to_min().optimize(optfunc, init, optbounds);
    Result result_max = opt.to_max().optimize(optfunc, init, optbounds);

    check_opt_result(result_min.score, -1., 1e-2, 1e-4);
    check_opt_result(result_max.score,  1., 1e-2, 1e-4);
}

template<class Opt> void test_sphere_func(Opt &&opt)
{
    using namespace Slic3r::opt;

    Result result = opt.to_min().optimize([](const auto &in) {
        auto [x, y] = in;

        return x * x + y * y + 1.;
    }, initvals({.6, -0.2}), bounds({{-1., 1.}, {-1., 1.}}));

    check_opt_result(result.score, 1., 1e-2, 1e-4);
}

TEST_CASE("Test brute force optimzer for basic 1D and 2D functions", "[Opt]") {
    using namespace Slic3r::opt;

    Optimizer<AlgBruteForce> opt;

    test_sin(opt);
    test_sphere_func(opt);
}
