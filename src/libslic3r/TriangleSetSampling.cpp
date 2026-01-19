#include "TriangleSetSampling.hpp"
#include <map>
#include <random>
#include <tbb/parallel_for.h>
#include <tbb/blocked_range.h>

namespace Slic3r {

TriangleSetSamples sample_its_uniform_parallel(size_t samples_count, const indexed_triangle_set &triangle_set) {
    std::vector<double> triangles_area(triangle_set.indices.size());

    tbb::parallel_for(tbb::blocked_range<size_t>(0, triangle_set.indices.size()),
            [&triangle_set, &triangles_area](
                    tbb::blocked_range<size_t> r) {
                for (size_t t_idx = r.begin(); t_idx < r.end(); ++t_idx) {
                    const Vec3f &a = triangle_set.vertices[triangle_set.indices[t_idx].x()];
                    const Vec3f &b = triangle_set.vertices[triangle_set.indices[t_idx].y()];
                    const Vec3f &c = triangle_set.vertices[triangle_set.indices[t_idx].z()];
                    double area = double(0.5 * (b - a).cross(c - a).norm());
                    triangles_area[t_idx] = area;
                }
            });

    std::map<double, size_t> area_sum_to_triangle_idx;
    float area_sum = 0;
    for (size_t t_idx = 0; t_idx < triangles_area.size(); ++t_idx) {
        area_sum += triangles_area[t_idx];
        area_sum_to_triangle_idx[area_sum] = t_idx;
    }

    std::mt19937_64 mersenne_engine { 27644437 };
    // random numbers on interval [0, 1)
    std::uniform_real_distribution<double> fdistribution;

    auto get_random = [&fdistribution, &mersenne_engine]() {
        return Vec3d { fdistribution(mersenne_engine), fdistribution(mersenne_engine), fdistribution(mersenne_engine) };
    };

    std::vector<Vec3d> random_samples(samples_count);
    std::generate(random_samples.begin(), random_samples.end(), get_random);

    TriangleSetSamples result;
    result.total_area = area_sum;
    result.positions.resize(samples_count);
    result.normals.resize(samples_count);
    result.triangle_indices.resize(samples_count);

    tbb::parallel_for(tbb::blocked_range<size_t>(0, samples_count),
            [&triangle_set, &area_sum_to_triangle_idx, &area_sum, &random_samples, &result](
                    tbb::blocked_range<size_t> r) {
                for (size_t s_idx = r.begin(); s_idx < r.end(); ++s_idx) {
                    double t_sample = random_samples[s_idx].x() * area_sum;
                    size_t t_idx = area_sum_to_triangle_idx.upper_bound(t_sample)->second;

                    double sq_u = std::sqrt(random_samples[s_idx].y());
                    double v = random_samples[s_idx].z();

                    Vec3f A = triangle_set.vertices[triangle_set.indices[t_idx].x()];
                    Vec3f B = triangle_set.vertices[triangle_set.indices[t_idx].y()];
                    Vec3f C = triangle_set.vertices[triangle_set.indices[t_idx].z()];

                    result.positions[s_idx] = A * (1 - sq_u) + B * (sq_u * (1 - v)) + C * (v * sq_u);
                    result.normals[s_idx] = ((B - A).cross(C - B)).normalized();
                    result.triangle_indices[s_idx] = t_idx;
                }
            });

    return result;
}

}
