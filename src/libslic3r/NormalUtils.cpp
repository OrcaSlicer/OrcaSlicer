#include "NormalUtils.hpp"

using namespace Slic3r;

Vec3f NormalUtils::create_triangle_normal(
    const stl_triangle_vertex_indices &indices,
    const std::vector<stl_vertex> &    vertices)
{
    const stl_vertex &v0        = vertices[indices[0]];
    const stl_vertex &v1        = vertices[indices[1]];
    const stl_vertex &v2        = vertices[indices[2]];
    Vec3f             direction = (v1 - v0).cross(v2 - v0);
    direction.normalize();
    return direction;
}

std::vector<Vec3f> NormalUtils::create_triangle_normals(
    const indexed_triangle_set &its)
{
    std::vector<Vec3f> normals;
    normals.reserve(its.indices.size());
    for (const auto &index : its.indices) {
        normals.push_back(create_triangle_normal(index, its.vertices));
    }
    return normals;
}

NormalUtils::Normals NormalUtils::create_normals_average_neighbor(
    const indexed_triangle_set &its)
{
    size_t             count_vertices = its.vertices.size();
    std::vector<Vec3f> normals(count_vertices, Vec3f(.0f, .0f, .0f));
    std::vector<unsigned int> count(count_vertices, 0);
    for (const auto &indice : its.indices) {
        Vec3f normal = create_triangle_normal(indice, its.vertices);
        for (int i = 0; i < 3; ++i) {
            normals[indice[i]] += normal;
            ++count[indice[i]];
        }
    }
    // normalize to size 1
    for (auto &normal : normals) {
        size_t index = &normal - &normals.front();
        normal /= static_cast<float>(count[index]);
    }
    return normals;
}

// calc triangle angle of vertex defined by index to triangle indices
float NormalUtils::indice_angle(int                            i,
                                const Vec3i32 &                indice,
                                const std::vector<stl_vertex> &vertices)
{
    int i1 = (i == 0) ? 2 : (i - 1);
    int i2 = (i == 2) ? 0 : (i + 1);

    Vec3f v1 = vertices[i1] - vertices[i];
    Vec3f v2 = vertices[i2] - vertices[i];

    v1.normalize();
    v2.normalize();

    float w = v1.dot(v2);
    if (w > 1.f)
        w = 1.f;
    else if (w < -1.f)
        w = -1.f;
    return acos(w);
}

NormalUtils::Normals NormalUtils::create_normals_angle_weighted(
    const indexed_triangle_set &its)
{
    size_t             count_vertices = its.vertices.size();
    std::vector<Vec3f> normals(count_vertices, Vec3f(.0f, .0f, .0f));
    std::vector<float> count(count_vertices, 0.f);
    for (const auto &indice : its.indices) {
        Vec3f normal = create_triangle_normal(indice, its.vertices);
        Vec3f angles(indice_angle(0, indice, its.vertices),
                     indice_angle(1, indice, its.vertices), 0.f);
        angles[2] = (M_PI - angles[0] - angles[1]);
        for (int i = 0; i < 3; ++i) {
            const float &weight = angles[i];
            normals[indice[i]] += normal * weight;
            count[indice[i]] += weight;
        }
    }
    // normalize to size 1
    for (auto &normal : normals) {
        size_t index = &normal - &normals.front();
        normal /= count[index];
    }
    return normals;
}

NormalUtils::Normals NormalUtils::create_normals_nelson_weighted(
    const indexed_triangle_set &its)
{
    size_t             count_vertices = its.vertices.size();
    std::vector<Vec3f> normals(count_vertices, Vec3f(.0f, .0f, .0f));
    std::vector<float> count(count_vertices, 0.f);
    const std::vector<stl_vertex> &vertices = its.vertices;
    for (const auto &indice : its.indices) {
        Vec3f normal = create_triangle_normal(indice, vertices);

        const stl_vertex &v0 = vertices[indice[0]];
        const stl_vertex &v1 = vertices[indice[1]];
        const stl_vertex &v2 = vertices[indice[2]];

        float e0 = (v0 - v1).norm();
        float e1 = (v1 - v2).norm();
        float e2 = (v2 - v0).norm();

        Vec3f coefs(e0 * e2, e0 * e1, e1 * e2);
        for (int i = 0; i < 3; ++i) {
            const float &weight = coefs[i];
            normals[indice[i]] += normal * weight;
            count[indice[i]] += weight;
        }
    }
    // normalize to size 1
    for (auto &normal : normals) {
        size_t index = &normal - &normals.front();
        normal /= count[index];
    }
    return normals;
}

// calculate normals by averaging normals of neghbor triangles
std::vector<Vec3f> NormalUtils::create_normals(
    const indexed_triangle_set &its, VertexNormalType type)
{
    switch (type) {
    case VertexNormalType::AverageNeighbor:
        return create_normals_average_neighbor(its);
    case VertexNormalType::AngleWeighted:
        return create_normals_angle_weighted(its);
    case VertexNormalType::NelsonMaxWeighted:
    default:
        return create_normals_nelson_weighted(its);
    }
}
