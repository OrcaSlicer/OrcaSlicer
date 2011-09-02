#include "ShortEdgeCollapse.hpp"
#include "libslic3r/NormalUtils.hpp"

#include <unordered_map>
#include <unordered_set>
#include <random>
#include <algorithm>

namespace Slic3r {

void its_short_edge_collpase(indexed_triangle_set &mesh, size_t target_triangle_count) {
    // whenever vertex is removed, its mapping is update to the index of vertex with wich it merged
    std::vector<size_t> vertices_index_mapping(mesh.vertices.size());
    for (size_t idx = 0; idx < vertices_index_mapping.size(); ++idx) {
        vertices_index_mapping[idx] = idx;
    }
    // Algorithm uses get_final_index query to get the actual vertex index. The query also updates all mappings on the way, essentially flattening the mapping
    std::vector<size_t> flatten_queue;
    auto get_final_index = [&vertices_index_mapping, &flatten_queue](const size_t &orig_index) {
        flatten_queue.clear();
        size_t idx = orig_index;
        while (vertices_index_mapping[idx] != idx) {
            flatten_queue.push_back(idx);
            idx = vertices_index_mapping[idx];
        }
        for (size_t i : flatten_queue) {
            vertices_index_mapping[i] = idx;
        }
        return idx;

    };

    // if face is removed, mark it here
    std::vector<bool> face_removal_flags(mesh.indices.size(), false);

    std::vector<Vec3i32> triangles_neighbors = its_face_neighbors_par(mesh);

    // now compute vertices dot product - this is used during edge collapse,
    // to determine which vertex to remove and which to keep;  We try to keep the one with larger angle, because it defines the shape "more".
    // The min vertex dot product is lowest dot product of its normal with the normals of faces around it.
    // the lower the dot product, the more we want to keep the vertex
    // NOTE: This score is not updated, even though the decimation does change the mesh. It saves computation time, and there are no strong reasons to update.
    std::vector<float> min_vertex_dot_product(mesh.vertices.size(), 1);
    {
        std::vector<Vec3f> face_normals = its_face_normals(mesh);
        std::vector<Vec3f> vertex_normals = NormalUtils::create_normals(mesh);

        for (size_t face_idx = 0; face_idx < mesh.indices.size(); ++face_idx) {
            Vec3i32 t = mesh.indices[face_idx];
            Vec3f n = face_normals[face_idx];
            min_vertex_dot_product[t[0]] = std::min(min_vertex_dot_product[t[0]], n.dot(vertex_normals[t[0]]));
            min_vertex_dot_product[t[1]] = std::min(min_vertex_dot_product[t[1]], n.dot(vertex_normals[t[1]]));
            min_vertex_dot_product[t[2]] = std::min(min_vertex_dot_product[t[2]], n.dot(vertex_normals[t[2]]));
        }
    }

    // lambda to remove face. It flags the face as removed, and updates neighbourhood info
    auto remove_face = [&triangles_neighbors, &face_removal_flags](int face_idx, int other_face_idx) {
        if (face_idx < 0) {
            return;
        }
        face_removal_flags[face_idx] = true;
        Vec3i32 neighbors = triangles_neighbors[face_idx];
        int n_a = neighbors[0] != other_face_idx ? neighbors[0] : neighbors[1];
        int n_b = neighbors[2] != other_face_idx ? neighbors[2] : neighbors[1];
        if (n_a > 0)
            for (int &n : triangles_neighbors[n_a]) {
                if (n == face_idx) {
                    n = n_b;
                    break;
                }
            }
        if (n_b > 0)
            for (int &n : triangles_neighbors[n_b]) {
                if (n == face_idx) {
                    n = n_a;
                    break;
                }
            }
    };

    std::mt19937_64 generator { 27644437 };// default constant seed! so that results are deterministic
    std::vector<size_t> face_indices(mesh.indices.size());
    for (size_t idx = 0; idx < face_indices.size(); ++idx) {
        face_indices[idx] = idx;
    }
    //tmp face indices used only for swapping
    std::vector<size_t> tmp_face_indices(mesh.indices.size());

    float decimation_ratio = 1.0f; // decimation ratio updated in each iteration. it is number of removed triangles / number of all
    float edge_len = 0.2f; // Allowed collapsible edge size. Starts low, but is gradually increased

    while (face_indices.size() > target_triangle_count) {
        // simpple func to increase the edge len - if decimation ratio is low, it increases the len up to twice, if decimation ratio is high, increments are low
        edge_len = edge_len * (1.0f + 1.0 - decimation_ratio);
        float max_edge_len_squared = edge_len * edge_len;

        //shuffle the faces and traverse in random order, this MASSIVELY improves the quality of the result
        std::shuffle(face_indices.begin(), face_indices.end(), generator);
        
        int allowed_face_removals = int(face_indices.size()) - int(target_triangle_count);
        for (const size_t &face_idx : face_indices) {
            if (face_removal_flags[face_idx]) {
                // if face already removed from previous collapses, skip (each collapse removes two triangles [at least] )
                continue;
            }

            // look at each edge if it is good candidate for collapse
            for (size_t edge_idx = 0; edge_idx < 3; ++edge_idx) {
                size_t vertex_index_keep = get_final_index(mesh.indices[face_idx][edge_idx]);
                size_t vertex_index_remove = get_final_index(mesh.indices[face_idx][(edge_idx + 1) % 3]);
                //check distance, skip long edges
                if ((mesh.vertices[vertex_index_keep] - mesh.vertices[vertex_index_remove]).squaredNorm()
                        > max_edge_len_squared) {
                    continue;
                }
                // swap indexes if vertex_index_keep has higher dot product (we want to keep low dot product vertices)
                if (min_vertex_dot_product[vertex_index_remove] < min_vertex_dot_product[vertex_index_keep]) {
                    size_t tmp = vertex_index_keep;
                    vertex_index_keep = vertex_index_remove;
                    vertex_index_remove = tmp;
                }

                //remove vertex
                {
                    // map its index to the index of the kept vertex
                    vertices_index_mapping[vertex_index_remove] = vertices_index_mapping[vertex_index_keep];
                }

                int neighbor_to_remove_face_idx = triangles_neighbors[face_idx][edge_idx];
                // remove faces
                remove_face(face_idx, neighbor_to_remove_face_idx);
                remove_face(neighbor_to_remove_face_idx, face_idx);
                allowed_face_removals-=2;

                // break. this triangle is done
                break;
            }

            if (allowed_face_removals <= 0) { break; }
        }

        // filter face_indices, remove those that have been collapsed
        size_t prev_size = face_indices.size();
        tmp_face_indices.clear();
        for (size_t face_idx : face_indices) {
            if (!face_removal_flags[face_idx]){
                tmp_face_indices.push_back(face_idx);
            }
        }
        face_indices.swap(tmp_face_indices);

        decimation_ratio = float(prev_size - face_indices.size()) / float(prev_size);
        //std::cout << " DECIMATION RATIO: " << decimation_ratio << std::endl;
    }

    //Extract the result mesh
    std::unordered_map<size_t, size_t> final_vertices_mapping;
    std::vector<Vec3f> final_vertices;
    std::vector<Vec3i32> final_indices;
    final_indices.reserve(face_indices.size());
    for (size_t idx : face_indices) {
        Vec3i32 final_face;
        for (size_t i = 0; i < 3; ++i) {
            final_face[i] = get_final_index(mesh.indices[idx][i]);
        }
        if (final_face[0] == final_face[1] || final_face[1] == final_face[2] || final_face[2] == final_face[0]) {
            continue; // discard degenerate triangles
        }

        for (size_t i = 0; i < 3; ++i) {
            if (final_vertices_mapping.find(final_face[i]) == final_vertices_mapping.end()) {
                final_vertices_mapping[final_face[i]] = final_vertices.size();
                final_vertices.push_back(mesh.vertices[final_face[i]]);
            }
            final_face[i] = final_vertices_mapping[final_face[i]];
        }

        final_indices.push_back(final_face);
    }

    mesh.vertices = final_vertices;
    mesh.indices = final_indices;
}

} //namespace Slic3r

