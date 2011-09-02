#include "QuadricEdgeCollapse.hpp"
#include <tuple>
#include <optional>
#include "MutablePriorityQueue.hpp"
#include <tbb/parallel_for.h>

using namespace Slic3r;

#ifndef NDEBUG
//    #define EXPENSIVE_DEBUG_CHECKS
#endif // NDEBUG

// only private namespace not neccessary be in .hpp
namespace QuadricEdgeCollapse {
    // SymetricMatrix
    class SymMat {
        using T = double;
        static const constexpr size_t N = 10;
        T m[N];
    public:    
        explicit SymMat(ArithmeticOnly<T> c = T()) { std::fill(m, m + N, c); }
    
        // Make plane
        SymMat(T a, T b, T c, T d)
        {
            m[0] = a * a; m[1] = a * b; m[2] = a * c; m[3] = a * d;
            m[4] = b * b; m[5] = b * c; m[6] = b * d;
            m[7] = c * c; m[8] = c * d;
            m[9] = d * d;
        }
    
        T operator[](int c) const { return m[c]; }
    
        // Determinant
        T det(int a11, int a12, int a13,
              int a21, int a22, int a23,
              int a31, int a32, int a33) const
        {
            T det = m[a11] * m[a22] * m[a33] + m[a13] * m[a21] * m[a32] +
                    m[a12] * m[a23] * m[a31] - m[a13] * m[a22] * m[a31] -
                    m[a11] * m[a23] * m[a32] - m[a12] * m[a21] * m[a33];
        
            return det;
        }
    
        const SymMat &operator+=(const SymMat &n)
        {
            for (size_t i = 0; i < N; ++i) m[i] += n[i];
            return *this;
        }    
    };

    using Vertices = std::vector<stl_vertex>;
    using Triangle = stl_triangle_vertex_indices;
    using Indices = std::vector<stl_triangle_vertex_indices>;
    using ThrowOnCancel = std::function<void(void)>;
    using StatusFn = std::function<void(int)>;
    // smallest error caused by edges, identify smallest edge in triangle
    struct Error
    {
        float value = -1.; // identifying of smallest edge is stored inside of TriangleInfo
        uint32_t triangle_index = 0;
        Error(float value, uint32_t triangle_index)
            : value(value)
            , triangle_index(triangle_index)
        {}
        Error() = default;
    };
    using Errors = std::vector<Error>;

    // merge information together - faster access during processing
    struct TriangleInfo {
        Vec3f n; // normalized normal - used for check when fliped

        // range(0 .. 2), 
        unsigned char min_index = 0; // identify edge for minimal Error -> lightweight Error structure

        TriangleInfo() = default;
        bool is_deleted() const { return n.x() > 2.f; }
        void set_deleted() { n.x() = 3.f; } 
    };  
    using TriangleInfos = std::vector<TriangleInfo>;
    struct VertexInfo {
        SymMat q; // sum quadric of surround triangles
        uint32_t start = 0, count = 0; // vertex neighbor triangles
        VertexInfo() = default;
        bool is_deleted() const { return count == 0; }
    };
    using VertexInfos = std::vector<VertexInfo>;
    struct EdgeInfo {
        uint32_t t_index=0; // triangle index
        unsigned char edge = 0; // 0 or 1 or 2
        EdgeInfo() = default;
    };
    using EdgeInfos = std::vector<EdgeInfo>;

    // DTO for change neighbors
    struct CopyEdgeInfo {
        uint32_t start;
        uint32_t count;
        uint32_t move;
        CopyEdgeInfo(uint32_t start, uint32_t count, uint32_t move)
            : start(start), count(count), move(move)
        {}
    };
    using CopyEdgeInfos = std::vector<CopyEdgeInfo>;

    Vec3d create_normal(const Triangle &triangle, const Vertices &vertices);
    std::array<Vec3d,3> create_vertices(uint32_t id_v1, uint32_t id_v2, const Vertices &vertices);
    std::array<double, 3> vertices_error(const SymMat &q, const std::array<Vec3d, 3> &vertices);
    double calculate_determinant(const SymMat &q);
    double calculate_error(uint32_t id_v1, uint32_t id_v2, const SymMat & q, const Vertices &vertices);
    Vec3f calculate_vertex(uint32_t id_v1, uint32_t id_v2, const SymMat & q, const Vertices &vertices);
    Vec3d calculate_vertex(double det, const SymMat &q);
    // calculate error for vertex and quadrics, triangle quadrics and triangle vertex give zero, only pozitive number
    double vertex_error(const SymMat &q, const Vec3d &vertex);
    SymMat create_quadric(const Triangle &t, const Vec3d& n, const Vertices &vertices);
    std::tuple<TriangleInfos, VertexInfos, EdgeInfos, Errors> 
    init(const indexed_triangle_set &its, ThrowOnCancel& throw_on_cancel, StatusFn& status_fn);
    std::optional<uint32_t> find_triangle_index1(uint32_t vi, const VertexInfo& v_info,
        uint32_t ti, const EdgeInfos& e_infos, const Indices& indices);
    void reorder_edges(EdgeInfos &e_infos, const VertexInfo &v_info, uint32_t ti0, uint32_t ti1);
    bool is_flipped(const Vec3f &new_vertex, uint32_t ti0, uint32_t ti1, const VertexInfo& v_info, 
        const TriangleInfos &t_infos, const EdgeInfos &e_infos, const indexed_triangle_set &its);
    bool degenerate(uint32_t vi, uint32_t ti0, uint32_t ti1, const VertexInfo &v_info, 
        const EdgeInfos &e_infos, const Indices &indices);
    bool create_no_volume(uint32_t vi0, uint32_t vi1, uint32_t ti0, uint32_t ti1,
        const VertexInfo &v_info0, const VertexInfo &v_info1, const EdgeInfos &e_infos, const Indices &indices);
    // find edge with smallest error in triangle
    Vec3d calculate_3errors(const Triangle &t, const Vertices &vertices, const VertexInfos &v_infos);
    Error calculate_error(uint32_t ti, const Triangle& t,const Vertices &vertices, const VertexInfos& v_infos, unsigned char& min_index);
    void remove_triangle(EdgeInfos &e_infos, VertexInfo &v_info, uint32_t ti);
    void change_neighbors(EdgeInfos &e_infos, VertexInfos &v_infos, uint32_t ti0, uint32_t ti1,
                          uint32_t vi0, uint32_t vi1, uint32_t vi_top0,
                          const Triangle &t1, CopyEdgeInfos& infos, EdgeInfos &e_infos1);
    void compact(const VertexInfos &v_infos, const TriangleInfos &t_infos, const EdgeInfos &e_infos, indexed_triangle_set &its);

#ifdef EXPENSIVE_DEBUG_CHECKS
    void store_surround(const char *obj_filename, size_t triangle_index, int depth, const indexed_triangle_set &its,
                        const VertexInfos &v_infos, const EdgeInfos &e_infos);
    bool check_neighbors(const indexed_triangle_set &its, const TriangleInfos &t_infos,
                         const VertexInfos &v_infos, const EdgeInfos &e_infos);
#endif /* EXPENSIVE_DEBUG_CHECKS */

    // constants --> may be move to config
    const uint32_t check_cancel_period = 16; // how many edge to reduce before call throw_on_cancel
    const size_t max_triangle_count_for_one_vertex = 50;
    // change speed of progress bargraph
    const int status_init_size = 10; // in percents
    // parts of init size
    const int status_normal_size = 25;
    const int status_sum_quadric = 25;
    const int status_set_offsets = 10;
    const int status_calc_errors = 30;
    const int status_create_refs = 10;
    } // namespace QuadricEdgeCollapse

using namespace QuadricEdgeCollapse;

void Slic3r::its_quadric_edge_collapse(
    indexed_triangle_set &    its,
    uint32_t                  triangle_count,
    float *                   max_error,
    std::function<void(void)> throw_on_cancel,
    std::function<void(int)>  status_fn)
{
    // check input
    if (triangle_count >= its.indices.size()) return;
    float maximal_error = (max_error == nullptr)? std::numeric_limits<float>::max() : *max_error;
    if (maximal_error <= 0.f) return;
    if (throw_on_cancel == nullptr) throw_on_cancel = []() {};
    if (status_fn == nullptr) status_fn = [](int) {};

    StatusFn init_status_fn = [&](int percent) {
        float n_percent = percent * status_init_size / 100.f;
        status_fn(static_cast<int>(std::round(n_percent)));
    };

    TriangleInfos t_infos; // only normals with information about deleted triangle
    VertexInfos   v_infos;
    EdgeInfos     e_infos;
    Errors        errors;
    std::tie(t_infos, v_infos, e_infos, errors) = init(its, throw_on_cancel, init_status_fn);
    throw_on_cancel();
    status_fn(status_init_size);

    //its_store_triangle(its, "triangle.obj", 1182);
    //store_surround("triangle_surround1.obj", 1182, 1, its, v_infos, e_infos);

    // convert from triangle index to mutable priority queue index
    std::vector<size_t> ti_2_mpqi(its.indices.size(), {0});
    auto setter = [&ti_2_mpqi](const Error &e, size_t index) { ti_2_mpqi[e.triangle_index] = index; };
    auto less = [](const Error &e1, const Error &e2) -> bool { return e1.value < e2.value; };
    auto mpq = make_miniheap_mutable_priority_queue<Error, 32, false>(std::move(setter), std::move(less)); 
    //MutablePriorityQueue<Error, decltype(setter), decltype(less)> mpq(std::move(setter), std::move(less));
    mpq.reserve(its.indices.size());
    for (Error &error :errors) mpq.push(error);

    CopyEdgeInfos ceis;
    ceis.reserve(max_triangle_count_for_one_vertex);
    EdgeInfos e_infos_swap;
    e_infos_swap.reserve(max_triangle_count_for_one_vertex);
    std::vector<uint32_t> changed_triangle_indices;
    changed_triangle_indices.reserve(2 * max_triangle_count_for_one_vertex);

    uint32_t actual_triangle_count = its.indices.size();
    uint32_t count_triangle_to_reduce = actual_triangle_count - triangle_count;
    auto increase_status = [&]() { 
        double reduced = (actual_triangle_count - triangle_count) /
                         (double) count_triangle_to_reduce;
        double status = status_init_size + (100 - status_init_size) *
                        (1. - reduced);            
        status_fn(static_cast<int>(std::round(status)));
    };
    // modulo for update status, call each percent only once
    uint32_t status_mod = std::max(uint32_t(16), 
        count_triangle_to_reduce / (100 - status_init_size));

    uint32_t iteration_number = 0;
    float last_collapsed_error = 0.f;
    while (actual_triangle_count > triangle_count && !mpq.empty()) {
        ++iteration_number;
        if (iteration_number % status_mod == 0) increase_status();
        if (iteration_number % check_cancel_period == 0) throw_on_cancel();

        // triangle index 0
        Error e = mpq.top(); // copy
        if (e.value >= maximal_error) break; // Too big error
        mpq.pop();
        uint32_t ti0 = e.triangle_index;
        TriangleInfo &t_info0 = t_infos[ti0];
        if (t_info0.is_deleted()) continue;
        assert(t_info0.min_index < 3);

        const Triangle &t0 = its.indices[ti0];
        uint32_t vi0 = t0[t_info0.min_index];
        uint32_t vi1 = t0[(t_info0.min_index+1) %3];
        // Need by move of neighbor edge infos in function: change_neighbors
        if (vi0 > vi1) std::swap(vi0, vi1);
        VertexInfo &v_info0 = v_infos[vi0];
        VertexInfo &v_info1 = v_infos[vi1];
        assert(!v_info0.is_deleted() && !v_info1.is_deleted());
        
        // new vertex position
        SymMat q(v_info0.q);
        q += v_info1.q;
        Vec3f new_vertex0 = calculate_vertex(vi0, vi1, q, its.vertices);
        // set of triangle indices that change quadric
        uint32_t ti1 = -1; // triangle 1 index
        auto ti1_opt = (v_info0.count < v_info1.count)?
            find_triangle_index1(vi1, v_info0, ti0, e_infos, its.indices) :
            find_triangle_index1(vi0, v_info1, ti0, e_infos, its.indices) ;
        if (ti1_opt.has_value()) { 
            ti1 = *ti1_opt;
            reorder_edges(e_infos, v_info0, ti0, ti1);
            reorder_edges(e_infos, v_info1, ti0, ti1);
        }
        if (!ti1_opt.has_value() || // edge has only one triangle
            degenerate(vi0, ti0, ti1, v_info1, e_infos, its.indices) ||
            degenerate(vi1, ti0, ti1, v_info0, e_infos, its.indices) ||
            create_no_volume(vi0, vi1, ti0, ti1, v_info0, v_info1, e_infos, its.indices) ||
            is_flipped(new_vertex0, ti0, ti1, v_info0, t_infos, e_infos, its) ||
            is_flipped(new_vertex0, ti0, ti1, v_info1, t_infos, e_infos, its)) {
            // try other triangle's edge
            Vec3d errors = calculate_3errors(t0, its.vertices, v_infos);
            Vec3i32 ord = (errors[0] < errors[1]) ? 
                ((errors[0] < errors[2])? 
                    ((errors[1] < errors[2]) ? Vec3i32(0, 1, 2) : Vec3i32(0, 2, 1)) :
                    Vec3i32(2, 0, 1)):
                ((errors[1] < errors[2])?
                    ((errors[0] < errors[2]) ? Vec3i32(1, 0, 2) : Vec3i32(1, 2, 0)) :
                    Vec3i32(2, 1, 0));
            if (t_info0.min_index == ord[0]) { 
                t_info0.min_index = ord[1];
                e.value = errors[t_info0.min_index];
            } else if (t_info0.min_index == ord[1]) {
                t_info0.min_index = ord[2];
                e.value = errors[t_info0.min_index];
            } else {
                // error is changed when surround edge is reduced
                t_info0.min_index = 3; // bad index -> invalidate
                e.value           = maximal_error;
            }
            // IMPROVE: check mpq top if it is ti1 with same edge
            mpq.push(e);
            continue;
        }
        
        last_collapsed_error = e.value;
        changed_triangle_indices.clear();
        changed_triangle_indices.reserve(v_info0.count + v_info1.count - 4);
        
        // for each vertex0 triangles
        uint32_t v_info0_end = v_info0.start + v_info0.count - 2;
        for (uint32_t di = v_info0.start; di < v_info0_end; ++di) {
            assert(di < e_infos.size());
            uint32_t    ti     = e_infos[di].t_index;
            changed_triangle_indices.emplace_back(ti);
        }

        // for each vertex1 triangles
        uint32_t v_info1_end = v_info1.start + v_info1.count - 2;
        for (uint32_t di = v_info1.start; di < v_info1_end; ++di) {
            assert(di < e_infos.size());
            EdgeInfo &e_info = e_infos[di];
            uint32_t    ti     = e_info.t_index;
            Triangle &t = its.indices[ti];
            t[e_info.edge] = vi0; // change index
            changed_triangle_indices.emplace_back(ti);
        }
        v_info0.q = q;

        // fix neighbors      
        // vertex index of triangle 0 which is not vi0 nor vi1
        uint32_t vi_top0 = t0[(t_info0.min_index + 2) % 3];
        const Triangle &t1 = its.indices[ti1];
        change_neighbors(e_infos, v_infos, ti0, ti1, vi0, vi1,
            vi_top0, t1, ceis, e_infos_swap);
        
        // Change vertex
        its.vertices[vi0] = new_vertex0;

        // fix errors - must be after set neighbors - v_infos
        mpq.remove(ti_2_mpqi[ti1]);
        for (uint32_t ti : changed_triangle_indices) {
            size_t priority_queue_index = ti_2_mpqi[ti];
            TriangleInfo& t_info = t_infos[ti];
            t_info.n = create_normal(its.indices[ti], its.vertices).cast<float>(); // recalc normals
            mpq[priority_queue_index] = calculate_error(ti, its.indices[ti], its.vertices, v_infos, t_info.min_index);
            mpq.update(priority_queue_index);
        }

        // set triangle(0 + 1) indices as deleted
        TriangleInfo &t_info1 = t_infos[ti1];
        t_info0.set_deleted();
        t_info1.set_deleted();
        // triangle counter decrementation
        actual_triangle_count-=2;
#ifdef EXPENSIVE_DEBUG_CHECKS
        assert(check_neighbors(its, t_infos, v_infos, e_infos));
#endif // EXPENSIVE_DEBUG_CHECKS
    }

    // compact triangle
    compact(v_infos, t_infos, e_infos, its);
    if (max_error != nullptr) *max_error = last_collapsed_error;
}

Vec3d QuadricEdgeCollapse::create_normal(const Triangle &triangle,
                                         const Vertices &vertices)
{
    Vec3d v0 = vertices[triangle[0]].cast<double>();
    Vec3d v1 = vertices[triangle[1]].cast<double>();
    Vec3d v2 = vertices[triangle[2]].cast<double>();
    // n = triangle normal
    Vec3d n = (v1 - v0).cross(v2 - v0);
    n.normalize();
    return n;
}

double QuadricEdgeCollapse::calculate_determinant(const SymMat &q)
{
    return q.det(0, 1, 2, 1, 4, 5, 2, 5, 7);
}

Vec3d QuadricEdgeCollapse::calculate_vertex(double det, const SymMat &q) {
    double det_1 = -1 / det;
    double det_x = q.det(1, 2, 3, 4, 5, 6, 5, 7, 8); // vx = A41/det(q_delta)
    double det_y = q.det(0, 2, 3, 1, 5, 6, 2, 7, 8); // vy = A42/det(q_delta)
    double det_z = q.det(0, 1, 3, 1, 4, 6, 2, 5, 8); // vz = A43/det(q_delta)
    return Vec3d(det_1 * det_x, -det_1 * det_y, det_1 * det_z);
}

std::array<Vec3d,3> QuadricEdgeCollapse::create_vertices(uint32_t id_v1, uint32_t id_v2, const Vertices &vertices)
{
    Vec3d v0 = vertices[id_v1].cast<double>();
    Vec3d v1 = vertices[id_v2].cast<double>();
    Vec3d vm = (v0 + v1) / 2.;
    return {v0, v1, vm};
}

std::array<double, 3> QuadricEdgeCollapse::vertices_error(
    const SymMat &q, const std::array<Vec3d, 3> &vertices)
{
    return {
        vertex_error(q, vertices[0]), 
        vertex_error(q, vertices[1]),
        vertex_error(q, vertices[2])};
}

double QuadricEdgeCollapse::calculate_error(uint32_t        id_v1,
                                            uint32_t        id_v2,
                                            const SymMat &  q,
                                            const Vertices &vertices)
{
    double det = calculate_determinant(q);
    if (std::abs(det) < std::numeric_limits<double>::epsilon()) {
        // can't divide by zero
        auto verts  = create_vertices(id_v1, id_v2, vertices);
        auto errors = vertices_error(q, verts);
        return *std::min_element(std::begin(errors), std::end(errors));
    }
    Vec3d vertex = calculate_vertex(det, q);
    return vertex_error(q, vertex);
}

// similar as calculate error but focus on new vertex without calculation of error
Vec3f QuadricEdgeCollapse::calculate_vertex(uint32_t          id_v1,
                                            uint32_t          id_v2,
                                            const SymMat &        q,
                                            const Vertices &vertices)
{
    double det = calculate_determinant(q);
    if (std::abs(det) < std::numeric_limits<double>::epsilon()) {
        // can't divide by zero
        auto verts  = create_vertices(id_v1, id_v2, vertices);
        auto errors = vertices_error(q, verts);
        auto mit    = std::min_element(std::begin(errors), std::end(errors));
        return verts[mit - std::begin(errors)].cast<float>();
    }
    return calculate_vertex(det, q).cast<float>();
}

double QuadricEdgeCollapse::vertex_error(const SymMat &q, const Vec3d &vertex)
{
    const double &x = vertex.x(), &y = vertex.y(), &z = vertex.z();
    return q[0] * x * x + 2 * q[1] * x * y + 2 * q[2] * x * z +
           2 * q[3] * x + q[4] * y * y + 2 * q[5] * y * z +
           2 * q[6] * y + q[7] * z * z + 2 * q[8] * z + q[9];
}

SymMat QuadricEdgeCollapse::create_quadric(const Triangle &t,
                                           const Vec3d &   n,
                                           const Vertices &vertices)
{
    Vec3d v0 = vertices[t[0]].cast<double>();
    return SymMat(n.x(), n.y(), n.z(), -n.dot(v0));
}

std::tuple<TriangleInfos, VertexInfos, EdgeInfos, Errors> 
QuadricEdgeCollapse::init(const indexed_triangle_set &its, ThrowOnCancel& throw_on_cancel, StatusFn& status_fn)
{
    int status_offset = 0;
    TriangleInfos t_infos(its.indices.size());
    VertexInfos   v_infos(its.vertices.size());
    {
        std::vector<SymMat> triangle_quadrics(its.indices.size());
        // calculate normals
        tbb::parallel_for(tbb::blocked_range<size_t>(0, its.indices.size()),
        [&](const tbb::blocked_range<size_t> &range) {
            for (size_t i = range.begin(); i < range.end(); ++i) {
                const Triangle &t      = its.indices[i];
                TriangleInfo &  t_info = t_infos[i];
                Vec3d           normal = create_normal(t, its.vertices);
                t_info.n = normal.cast<float>();
                triangle_quadrics[i] = create_quadric(t, normal, its.vertices);
                if (i % 1000000 == 0) {
                    throw_on_cancel();
                    status_fn(status_offset + (i * status_normal_size) / its.indices.size());
                }
            }
        }); // END parallel for
        status_offset += status_normal_size;

        // sum quadrics
        for (size_t i = 0; i < its.indices.size(); i++) {
            const Triangle &t = its.indices[i];
            const SymMat &  q = triangle_quadrics[i];
            for (size_t e = 0; e < 3; e++) {
                VertexInfo &v_info = v_infos[t[e]];
                v_info.q += q;
                ++v_info.count; // triangle count
            }
            if (i % 1000000 == 0) {
                throw_on_cancel();
                status_fn(status_offset + (i * status_sum_quadric) / its.indices.size());
            }
        }
        status_offset += status_sum_quadric;
    } // remove triangle quadrics

    // set offseted starts
    uint32_t triangle_start = 0;
    for (VertexInfo &v_info : v_infos) {
        v_info.start = triangle_start;
        triangle_start += v_info.count;
        // set filled vertex to zero
        v_info.count = 0;
    }
    assert(its.indices.size() * 3 == triangle_start);

    status_offset += status_set_offsets;
    throw_on_cancel();
    status_fn(status_offset);

    // calc error
    Errors errors(its.indices.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, its.indices.size()),
    [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            const Triangle &t      = its.indices[i];
            TriangleInfo &  t_info = t_infos[i];
            errors[i] = calculate_error(i, t, its.vertices, v_infos, t_info.min_index);
            if (i % 1000000 == 0) {
                throw_on_cancel();
                status_fn(status_offset + (i * status_calc_errors) / its.indices.size());
            }
            if (i % 1000000 == 0) throw_on_cancel();
        }
    }); // END parallel for

    status_offset += status_calc_errors;
    
    // create reference
    EdgeInfos e_infos(its.indices.size() * 3);
    for (size_t i = 0; i < its.indices.size(); i++) {
        const Triangle &t = its.indices[i];       
        for (size_t j = 0; j < 3; ++j) {
            VertexInfo &v_info = v_infos[t[j]];
            size_t ei = v_info.start + v_info.count;
            assert(ei < e_infos.size());
            EdgeInfo &e_info = e_infos[ei];
            e_info.t_index  = i;
            e_info.edge      = j;
            ++v_info.count;
        }
        if (i % 1000000 == 0) {
            throw_on_cancel();
            status_fn(status_offset + (i * status_create_refs) / its.indices.size());
        }
    }

    throw_on_cancel();
    status_fn(100);
    return {t_infos, v_infos, e_infos, errors};
}

std::optional<uint32_t> QuadricEdgeCollapse::find_triangle_index1(uint32_t          vi,
                                                   const VertexInfo &v_info,
                                                   uint32_t          ti0,
                                                   const EdgeInfos & e_infos,
                                                   const Indices &   indices)
{
    coord_t vi_coord = static_cast<coord_t>(vi);
    uint32_t end = v_info.start + v_info.count;
    for (uint32_t ei = v_info.start; ei < end; ++ei) {
        const EdgeInfo &e_info = e_infos[ei];
        if (e_info.t_index == ti0) continue;
        const Triangle& t = indices[e_info.t_index];
        if (t[(e_info.edge + 1) % 3] == vi_coord || 
            t[(e_info.edge + 2) % 3] == vi_coord)
            return e_info.t_index;
    }
    // triangle0 is on border and do NOT have twin edge
    return {};
}

void QuadricEdgeCollapse::reorder_edges(EdgeInfos &       e_infos,
                                        const VertexInfo &v_info,
                                        uint32_t          ti0,
                                        uint32_t          ti1)
{
    // swap edge info of ti0 and ti1 to end(last one and one before)
    size_t v_info_end = v_info.start + v_info.count - 2;
    EdgeInfo &e_info_ti0 = e_infos[v_info_end]; 
    EdgeInfo &e_info_ti1 = e_infos[v_info_end+1]; 
    bool      is_swaped  = false;
    for (size_t ei = v_info.start; ei < v_info_end; ++ei) {
        EdgeInfo &e_info = e_infos[ei];
        if (e_info.t_index == ti0) {
            std::swap(e_info, e_info_ti0);
            if (is_swaped) return;
            if (e_info.t_index == ti1) { 
                std::swap(e_info, e_info_ti1);
                return;
            }
            is_swaped = true;
        } else if (e_info.t_index == ti1) {
            std::swap(e_info, e_info_ti1);
            if (is_swaped) return;
            if (e_info.t_index == ti0) {
                std::swap(e_info, e_info_ti0);
                return;
            }
            is_swaped = true;
        }
    }
}

bool QuadricEdgeCollapse::is_flipped(const Vec3f &               new_vertex,
                                     uint32_t                    ti0,
                                     uint32_t                    ti1,
                                     const VertexInfo &          v_info,
                                     const TriangleInfos &       t_infos,
                                     const EdgeInfos &           e_infos,
                                     const indexed_triangle_set &its)
{
    static const float thr_pos = 1.0f - std::numeric_limits<float>::epsilon();
    static const float thr_neg = -thr_pos;
    static const float dot_thr = 0.2f; // Value from simplify mesh cca 80 DEG

    // for each vertex triangles
    size_t v_info_end = v_info.start + v_info.count-2;
    for (size_t ei = v_info.start; ei < v_info_end; ++ei) {
        assert(ei < e_infos.size());
        const EdgeInfo &e_info = e_infos[ei];
        const Triangle &t      = its.indices[e_info.t_index];
        const Vec3f &normal = t_infos[e_info.t_index].n;
        const Vec3f &vf     = its.vertices[t[(e_info.edge + 1) % 3]];
        const Vec3f &vs     = its.vertices[t[(e_info.edge + 2) % 3]];

        Vec3f d1 = vf - new_vertex;
        d1.normalize();
        Vec3f d2 = vs - new_vertex;
        d2.normalize();

        float dot = d1.dot(d2);
        if (dot > thr_pos || dot < thr_neg) return true;
        // IMPROVE: propagate new normal
        Vec3f n = d1.cross(d2);
        n.normalize(); 
        if(n.dot(normal) < dot_thr) return true;
    }
    return false;
}

bool QuadricEdgeCollapse::degenerate(uint32_t          vi,
                                     uint32_t          ti0,
                                     uint32_t          ti1,
                                     const VertexInfo &v_info,
                                     const EdgeInfos & e_infos,
                                     const Indices &   indices)
{
    // check surround triangle do not contain vertex index
    // protect from creation of triangle with two same vertices inside
    size_t v_info_end = v_info.start + v_info.count - 2;
    for (size_t ei = v_info.start; ei < v_info_end; ++ei) {
        assert(ei < e_infos.size());
        const EdgeInfo &e_info = e_infos[ei];
        const Triangle &t = indices[e_info.t_index];
        for (size_t i = 0; i < 3; ++i)
            if (static_cast<uint32_t>(t[i]) == vi) return true;
    }
    return false;
}

bool QuadricEdgeCollapse::create_no_volume(
    uint32_t          vi0    , uint32_t          vi1,
    uint32_t          ti0    , uint32_t          ti1,
    const VertexInfo &v_info0, const VertexInfo &v_info1,
    const EdgeInfos & e_infos, const Indices &indices)
{
    // check that triangles around vertex0 doesn't have half edge
    // with opposit order in set of triangles around vertex1
    // protect from creation of two triangles with oposit order - no volume space
    size_t v_info0_end = v_info0.start + v_info0.count - 2;
    size_t v_info1_end = v_info1.start + v_info1.count - 2;
    for (size_t ei0 = v_info0.start; ei0 < v_info0_end; ++ei0) {
        const EdgeInfo &e_info0 = e_infos[ei0];
        const Triangle &t0 = indices[e_info0.t_index];
        // edge CCW vertex indices are t0vi0, t0vi1
        size_t t0i = 0;
        uint32_t t0vi0 = static_cast<uint32_t>(t0[t0i]);
        if (t0vi0 == vi0) { 
            ++t0i; 
            t0vi0 = static_cast<uint32_t>(t0[t0i]);
        }
        ++t0i;
        uint32_t t0vi1 = static_cast<uint32_t>(t0[t0i]);
        if (t0vi1 == vi0) { 
            ++t0i;
            t0vi1 = static_cast<uint32_t>(t0[t0i]);
        }
        for (size_t ei1 = v_info1.start; ei1 < v_info1_end; ++ei1) {
            const EdgeInfo &e_info1 = e_infos[ei1];
            const Triangle &t1 = indices[e_info1.t_index];
            size_t t1i = 0;
            for (; t1i < 3; ++t1i) if (static_cast<uint32_t>(t1[t1i]) == t0vi1) break;            
            if (t1i >= 3) continue; // without vertex index from triangle 0
            // check if second index is same too
            ++t1i;
            if (t1i == 3) t1i = 0; // triangle loop(modulo 3)
            if (static_cast<uint32_t>(t1[t1i]) == vi1) { 
                ++t1i; 
                if (t1i == 3) t1i = 0; // triangle loop(modulo 3)
            }
            if (static_cast<uint32_t>(t1[t1i]) == t0vi0) return true;
        }
    }
    return false;
}

Vec3d QuadricEdgeCollapse::calculate_3errors(const Triangle &   t,
                                             const Vertices &   vertices,
                                             const VertexInfos &v_infos)
{
    Vec3d error;
    for (size_t j = 0; j < 3; ++j) {
        size_t   j2  = (j == 2) ? 0 : (j + 1);
        uint32_t vi0 = t[j];
        uint32_t vi1 = t[j2];
        SymMat   q(v_infos[vi0].q); // copy
        q += v_infos[vi1].q;
        error[j] = calculate_error(vi0, vi1, q, vertices);
    }
    return error;
}

Error QuadricEdgeCollapse::calculate_error(uint32_t           ti,
                                           const Triangle &   t,
                                           const Vertices &   vertices,
                                           const VertexInfos &v_infos,
                                           unsigned char &    min_index)
{
    Vec3d error = calculate_3errors(t, vertices, v_infos);
    // select min error
    min_index = (error[0] < error[1]) ? ((error[0] < error[2]) ? 0 : 2) :
                                        ((error[1] < error[2]) ? 1 : 2);
    return Error(static_cast<float>(error[min_index]), ti);    
}

void QuadricEdgeCollapse::remove_triangle(EdgeInfos & e_infos,
                                          VertexInfo &v_info,
                                          uint32_t      ti)
{
    auto e_info     = e_infos.begin() + v_info.start;
    auto e_info_end = e_info + v_info.count - 1;
    for (; e_info != e_info_end; ++e_info) {
        if (e_info->t_index == ti) {
            *e_info = *e_info_end;
            --v_info.count;
            return;
        }
    }    
    assert(e_info_end->t_index == ti);
    // last triangle is ti
    --v_info.count;
}

void QuadricEdgeCollapse::change_neighbors(EdgeInfos &     e_infos,
                                           VertexInfos &   v_infos,
                                           uint32_t          ti0,
                                           uint32_t          ti1,
                                           uint32_t          vi0,
                                           uint32_t          vi1,
                                           uint32_t          vi_top0,
                                           const Triangle &t1,
                                           CopyEdgeInfos& infos,
                                           EdgeInfos &     e_infos1)
{
    // have to copy Edge info from higher vertex index into smaller
    assert(vi0 < vi1);
    
    // vertex index of triangle 1 which is not vi0 nor vi1
    uint32_t vi_top1 = t1[0];
    if (vi_top1 == vi0 || vi_top1 == vi1) {
        vi_top1 = t1[1];
        if (vi_top1 == vi0 || vi_top1 == vi1) vi_top1 = t1[2];
    }

    remove_triangle(e_infos, v_infos[vi_top0], ti0);
    remove_triangle(e_infos, v_infos[vi_top1], ti1);

    VertexInfo &v_info0 = v_infos[vi0];
    VertexInfo &v_info1 = v_infos[vi1];

    uint32_t new_triangle_count = v_info0.count + v_info1.count - 4;
    remove_triangle(e_infos, v_info0, ti0);
    remove_triangle(e_infos, v_info0, ti1);

    // copy second's edge infos out of e_infos, to free size
    e_infos1.clear();
    e_infos1.reserve(v_info1.count - 2);
    uint32_t v_info_s_end = v_info1.start + v_info1.count;
    for (uint32_t ei = v_info1.start; ei < v_info_s_end; ++ei) {
        const EdgeInfo &e_info = e_infos[ei];
        if (e_info.t_index == ti0) continue;
        if (e_info.t_index == ti1) continue;
        e_infos1.emplace_back(e_info);
    }
    v_info1.count = 0;

    uint32_t need = (new_triangle_count < v_info0.count)? 0:
                  (new_triangle_count - v_info0.count);

    uint32_t      act_vi     = vi0 + 1;
    VertexInfo *act_v_info = &v_infos[act_vi];
    uint32_t      act_start  = act_v_info->start;
    uint32_t      last_end   = v_info0.start + v_info0.count;

    infos.clear();
    infos.reserve(need);

    while (true) {
        uint32_t save = act_start - last_end;
        if (save > 0) {
            if (save >= need) break;
            need -= save;
            infos.emplace_back(act_v_info->start, act_v_info->count, need);
        } else {
            infos.back().count += act_v_info->count;
        }
        last_end = act_v_info->start + act_v_info->count;
        act_v_info->start += need; 
        ++act_vi;
        if (act_vi < v_infos.size()) {
            act_v_info = &v_infos[act_vi];
            act_start  = act_v_info->start;
        } else
            act_start = e_infos.size(); // fix for edge between last two triangles
    }

    // copy by c_infos
    for (uint32_t i = infos.size(); i > 0; --i) {
        const CopyEdgeInfo &c_info = infos[i - 1];
        for (uint32_t ei = c_info.start + c_info.count - 1; ei >= c_info.start; --ei)
            e_infos[ei + c_info.move] = e_infos[ei]; // copy
    }

    // copy triangle from first info into second
    for (uint32_t ei_s = 0; ei_s < e_infos1.size(); ++ei_s) {
        uint32_t ei_f = v_info0.start + v_info0.count;
        e_infos[ei_f] = e_infos1[ei_s]; // copy
        ++v_info0.count;
    }
}

void QuadricEdgeCollapse::compact(const VertexInfos &   v_infos,
                                  const TriangleInfos & t_infos,
                                  const EdgeInfos &     e_infos,
                                  indexed_triangle_set &its)
{
    uint32_t vi_new = 0;
    for (uint32_t vi = 0; vi < v_infos.size(); ++vi) {
        const VertexInfo &v_info = v_infos[vi];
        if (v_info.is_deleted()) continue; // deleted
        uint32_t e_info_end = v_info.start + v_info.count;
        for (uint32_t ei = v_info.start; ei < e_info_end; ++ei) { 
            const EdgeInfo &e_info = e_infos[ei];
            // change vertex index
            its.indices[e_info.t_index][e_info.edge] = vi_new;
        }
        // compact vertices
        its.vertices[vi_new++] = its.vertices[vi];
    }
    // remove vertices tail
    its.vertices.erase(its.vertices.begin() + vi_new, its.vertices.end());

    uint32_t ti_new = 0;
    for (uint32_t ti = 0; ti < t_infos.size(); ti++) { 
        const TriangleInfo &t_info = t_infos[ti];
        if (t_info.is_deleted()) continue;
        its.indices[ti_new++] = its.indices[ti];
    }
    its.indices.erase(its.indices.begin() + ti_new, its.indices.end());
}

#ifdef EXPENSIVE_DEBUG_CHECKS

// store triangle surrounding to file
void QuadricEdgeCollapse::store_surround(const char *obj_filename,
                                         size_t      triangle_index,
                                         int         depth,
                                         const indexed_triangle_set &its,
                                         const VertexInfos &         v_infos,
                                         const EdgeInfos &           e_infos)
{
    std::set<size_t> triangles;
    //             triangle index, depth
    using Item = std::pair<size_t, int>;
    std::queue<Item> process;
    process.push({triangle_index, depth});

    while (!process.empty()) {
        Item item = process.front();
        process.pop();
        size_t ti = item.first;
        auto   it = triangles.find(ti);
        if (it != triangles.end()) continue;
        triangles.insert(ti);
        if (item.second == 0) continue;

        const Vec3i32 &t = its.indices[ti];
        for (size_t i = 0; i < 3; ++i) {
            const auto &v_info = v_infos[t[i]];
            for (size_t d = 0; d < v_info.count; ++d) {
                size_t      ei     = v_info.start + d;
                const auto &e_info = e_infos[ei];
                auto        it     = triangles.find(e_info.t_index);
                if (it != triangles.end()) continue;
                process.push({e_info.t_index, item.second - 1});
            }
        }
    }

    std::vector<size_t> trs;
    trs.reserve(triangles.size());
    for (size_t ti : triangles) trs.push_back(ti);
    its_store_triangles(its, obj_filename, trs);
    // its_write_obj(its,"original.obj");
}

bool QuadricEdgeCollapse::check_neighbors(const indexed_triangle_set &its,
                                          const TriangleInfos &       t_infos,
                                          const VertexInfos &         v_infos,
                                          const EdgeInfos &           e_infos)
{
    VertexInfos v_infos2(v_infos.size());
    size_t      count_indices = 0;

    for (size_t ti = 0; ti < its.indices.size(); ti++) {
        if (t_infos[ti].is_deleted()) continue;
        ++count_indices;
        const Triangle &t = its.indices[ti];
        for (size_t e = 0; e < 3; e++) {
            VertexInfo &v_info = v_infos2[t[e]];
            ++v_info.count; // triangle count
        }
    }

    uint32_t triangle_start = 0;
    for (VertexInfo &v_info : v_infos2) {
        v_info.start = triangle_start;
        triangle_start += v_info.count;
        // set filled vertex to zero
        v_info.count = 0;
    }

    // create reference
    EdgeInfos e_infos2(count_indices * 3);
    for (size_t ti = 0; ti < its.indices.size(); ti++) {
        if (t_infos[ti].is_deleted()) continue;
        const Triangle &t = its.indices[ti];
        for (size_t j = 0; j < 3; ++j) {
            VertexInfo &v_info = v_infos2[t[j]];
            size_t      ei     = v_info.start + v_info.count;
            assert(ei < e_infos2.size());
            EdgeInfo &e_info = e_infos2[ei];
            e_info.t_index   = ti;
            e_info.edge      = j;
            ++v_info.count;
        }
    }

    for (size_t vi = 0; vi < its.vertices.size(); vi++) {
        const VertexInfo &v_info = v_infos[vi];
        if (v_info.is_deleted()) continue;
        const VertexInfo &v_info2 = v_infos2[vi];
        if (v_info.count != v_info2.count) { return false; }
        EdgeInfos eis;
        eis.reserve(v_info.count);
        std::copy(e_infos.begin() + v_info.start,
                  e_infos.begin() + v_info.start + v_info.count,
                  std::back_inserter(eis));
        auto compare = [](const EdgeInfo &ei1, const EdgeInfo &ei2) {
            return ei1.t_index < ei2.t_index;
        };
        std::sort(eis.begin(), eis.end(), compare);
        std::sort(e_infos2.begin() + v_info2.start,
                  e_infos2.begin() + v_info2.start + v_info2.count, compare);
        for (size_t ei = 0; ei < v_info.count; ++ei) {
            if (eis[ei].t_index != e_infos2[ei + v_info2.start].t_index) {
                return false;
            }
        }
    }
    return true;
}
#endif /* EXPENSIVE_DEBUG_CHECKS */
