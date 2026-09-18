#include "SemanticMaterialRegions.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <tuple>
#include <Eigen/Eigenvalues>

namespace Slic3r::AI::SemanticColoring {
namespace {
constexpr uint32_t absent = std::numeric_limits<uint32_t>::max();
bool valid_color(const Color& color)
{
    return std::all_of(color.begin(), color.end(), [](float value) {
        return std::isfinite(value) && value >= 0.f && value <= 1.f;
    });
}
Color to_lab(Color color)
{
    for (float& channel : color)
        channel = channel <= .04045f ? channel / 12.92f : std::pow((channel + .055f) / 1.055f, 2.4f);
    const float l = std::cbrt(.4122214708f*color[0] + .5363325363f*color[1] + .0514459929f*color[2]);
    const float m = std::cbrt(.2119034982f*color[0] + .6806995451f*color[1] + .1073969566f*color[2]);
    const float s = std::cbrt(.0883024619f*color[0] + .2817188376f*color[1] + .6299787005f*color[2]);
    return {.2104542553f*l + .793617785f*m - .0040720468f*s,
        1.9779984951f*l - 2.428592205f*m + .4505937099f*s,
        .0259040371f*l + .7827717662f*m - .808675766f*s};
}
float chroma(const Color& color) { return std::hypot(color[1], color[2]); }
float appearance_distance(const Color& a, const Color& b)
{
    // For a skin-material gap, illumination-driven lightness is weaker evidence
    // than chromaticity. The general palette matcher keeps its normal weighting.
    const float lightness = (a[0] - b[0]) * .25f;
    const float red_green = a[1] - b[1], yellow_blue = a[2] - b[2];
    return lightness * lightness + red_green * red_green + yellow_blue * yellow_blue;
}
bool warm_skin_appearance(const Color& color)
{
    const float saturation = chroma(color);
    return color[0] >= .40f && color[0] <= .85f && saturation >= .025f && saturation <= .16f &&
        color[1] > .005f && color[2] > .005f && color[1] <= color[2] * 1.6f;
}
bool material_label(Label label)
{
    // The exclusion is unconditional: even uncertain eyes, skin and accessories
    // are barriers. A low confidence value is not permission to recolor them.
    return label == Label::Unknown || label == Label::Background ||
        label == Label::Hair || label == Label::Clothes;
}
bool face_detail_label(Label label)
{
    return label == Label::Lips || label == Label::MouthInterior ||
        label == Label::EyeSclera || label == Label::Iris ||
        label == Label::Eyebrow || label == Label::Accessories;
}
struct DisjointSet {
    std::vector<uint32_t> parent;
    explicit DisjointSet(size_t count) : parent(count) { std::iota(parent.begin(), parent.end(), 0); }
    uint32_t root(uint32_t value) {
        while (parent[value] != value) { parent[value] = parent[parent[value]]; value = parent[value]; }
        return value;
    }
    void join(uint32_t a, uint32_t b) { a = root(a); b = root(b); if (a != b) parent[std::max(a,b)] = std::min(a,b); }
};
struct Face {
    Color color {};
    Vec3f normal {Vec3f::Zero()}, center {Vec3f::Zero()};
    double area {0};
    int assigned {-1};
    bool allowed {false}, bright {false}, dark {false};
};
struct Edge { uint64_t key; uint32_t face; };
struct Region {
    std::vector<uint32_t> faces;
    double area {0};
};
void fill_supported_skin_gaps(const Analysis& analysis, const std::vector<Face>& faces,
                             const std::vector<std::pair<uint32_t,uint32_t>>& edges,
                             float maximum_surface_distance, std::vector<int>& refined)
{
    constexpr uint8_t unconditional_hops = 6;
    constexpr uint8_t maximum_hops = 16;
    const size_t count = faces.size();
    std::vector<uint32_t> owner(count,absent), proposed(count,absent);
    std::vector<uint8_t> hops(count,255), eligible(count,0), blocked(count,0);
    std::vector<float> cost(count,0.f), surface_distance(count,0.f), proposed_cost(count), proposed_distance(count);
    bool have_seed=false,have_gap=false;
    for (uint32_t id=0;id<count;++id) {
        const auto label=analysis.face_labels[id];
        if (!warm_skin_appearance(faces[id].color) || faces[id].area<=0) continue;
        if (label==Label::FaceSkin && analysis.face_confidence[id]>=minimum_confidence &&
            faces[id].assigned>=0) {
            owner[id]=id;hops[id]=0;have_seed=true;
        } else if (faces[id].assigned<0 && (label==Label::FaceSkin || label==Label::Unknown)) {
            eligible[id]=1;have_gap=true;
        }
    }
    if (!have_seed || !have_gap) return;
    // Detail masks can miss the outermost lip triangles. The conservative
    // surface collar prevents a skin donor from swallowing those unknown faces.
    std::vector<uint8_t> lip_distance(count,255);
    bool have_lips=false;
    for (uint32_t id=0;id<count;++id) if (analysis.face_confidence[id]>=minimum_confidence &&
        (analysis.face_labels[id]==Label::Lips || analysis.face_labels[id]==Label::MouthInterior)) {
        lip_distance[id]=0;have_lips=true;
    }
    if (have_lips) for (uint8_t step=0;step<6;++step) for (const auto& edge:edges) {
        if (lip_distance[edge.first]==step && lip_distance[edge.second]>step+1)
            lip_distance[edge.second]=step+1;
        if (lip_distance[edge.second]==step && lip_distance[edge.first]>step+1)
            lip_distance[edge.first]=step+1;
    }
    for (const auto& edge:edges) for (const auto& side:{edge,std::make_pair(edge.second,edge.first)}) {
        const uint32_t id=side.first,neighbor=side.second;
        const auto label=analysis.face_labels[neighbor];
        if (eligible[id] && analysis.face_confidence[neighbor]>=minimum_confidence &&
            label!=Label::FaceSkin && label!=Label::Unknown && label!=Label::Background)
            blocked[id]=1;
    }
    if (have_lips) for (uint32_t id=0;id<count;++id)
        if (eligible[id] && lip_distance[id]<=6) blocked[id]=1;
    // The original color must remain continuous both along the path and back
    // to its reliable skin seed. Always retain the six-edge feature-scale fill.
    // Denser meshes may continue within a bounded physical surface distance.
    for (uint8_t step=0;step<maximum_hops;++step) {
        std::fill(proposed.begin(),proposed.end(),absent);
        std::fill(proposed_cost.begin(),proposed_cost.end(),std::numeric_limits<float>::max());
        std::fill(proposed_distance.begin(),proposed_distance.end(),std::numeric_limits<float>::max());
        const auto visit=[&](uint32_t from,uint32_t to) {
            if (hops[from]!=step || !eligible[to] || blocked[to] || owner[to]!=absent) return;
            const uint32_t seed=owner[from];
            const float edge_cost=appearance_distance(faces[from].color,faces[to].color);
            const float total=cost[from]+edge_cost;
            const float distance=surface_distance[from]+(faces[from].center-faces[to].center).norm();
            if (edge_cost>.0009f || total>.0016f ||
                appearance_distance(faces[seed].color,faces[to].color)>.0025f ||
                (step>=unconditional_hops && distance>maximum_surface_distance)) return;
            if (total<proposed_cost[to] || (total==proposed_cost[to] &&
                (distance<proposed_distance[to] || (distance==proposed_distance[to] && seed<proposed[to])))) {
                proposed[to]=seed;proposed_cost[to]=total;proposed_distance[to]=distance;
            }
        };
        for (const auto& edge:edges) { visit(edge.first,edge.second);visit(edge.second,edge.first); }
        bool changed=false;
        for (uint32_t id=0;id<count;++id) if (proposed[id]!=absent) {
            owner[id]=proposed[id];hops[id]=step+1;cost[id]=proposed_cost[id];surface_distance[id]=proposed_distance[id];
            refined[id]=faces[owner[id]].assigned;changed=true;
        }
        if (!changed) break;
    }
}

void fill_isolated_assigned_skin_holes(const Analysis& analysis, const std::vector<Face>& faces,
                                       const std::vector<std::pair<uint32_t,uint32_t>>& edges,
                                       std::vector<int>& refined)
{
    const size_t count = faces.size();
    std::vector<uint8_t> skin_slots;
    for (uint32_t id = 0; id < count; ++id) {
        if (analysis.face_labels[id] != Label::FaceSkin ||
            analysis.face_confidence[id] < minimum_confidence || faces[id].assigned < 0 ||
            !warm_skin_appearance(faces[id].color)) continue;
        const size_t slot = size_t(faces[id].assigned);
        if (skin_slots.size() <= slot) skin_slots.resize(slot + 1, 0);
        skin_slots[slot] = 1;
    }
    if (skin_slots.empty()) return;

    std::vector<uint8_t> neighbors(count, 0), blocked(count, 0);
    std::vector<std::array<uint8_t, 6>> votes(count);
    const auto visit = [&](uint32_t id, uint32_t neighbor) {
        if (analysis.face_labels[id] != Label::Unknown || faces[id].assigned < 0 ||
            !warm_skin_appearance(faces[id].color)) return;
        ++neighbors[id];
        const Label label = analysis.face_labels[neighbor];
        if (analysis.face_confidence[neighbor] >= minimum_confidence &&
            label != Label::Unknown && label != Label::FaceSkin && label != Label::BodySkin) {
            blocked[id] = 1; return;
        }
        const int slot = refined[neighbor] >= 0 ? refined[neighbor] : faces[neighbor].assigned;
        if (slot < 0 || size_t(slot) >= skin_slots.size() || !skin_slots[size_t(slot)] ||
            appearance_distance(faces[id].color, faces[neighbor].color) > .0009f) return;
        ++votes[id][size_t(slot)];
    };
    for (const auto& edge : edges) { visit(edge.first, edge.second); visit(edge.second, edge.first); }
    for (uint32_t id = 0; id < count; ++id) {
        if (blocked[id] || neighbors[id] < 2) continue;
        const auto best = std::max_element(votes[id].begin(), votes[id].end());
        if (*best >= 2 && *best * 2 >= neighbors[id]) refined[id] = int(best - votes[id].begin());
    }
}

void fill_reliable_skin_shadow_holes(const Analysis& analysis, const std::vector<Face>& faces,
                                     const std::vector<std::pair<uint32_t,uint32_t>>& edges,
                                     const std::vector<std::pair<uint32_t,uint32_t>>& surface_edges,
                                     const std::vector<uint8_t>& point_detail_barrier,
                                     float detail_radius,
                                     std::vector<int>& refined)
{
    constexpr uint8_t maximum_steps = 3;
    constexpr float minimum_ear_fold_confidence = .35f;
    const size_t count = faces.size();
    std::vector<uint8_t> skin_slots;
    for (uint32_t id = 0; id < count; ++id) {
        if (analysis.face_labels[id] != Label::FaceSkin ||
            analysis.face_confidence[id] < minimum_confidence || faces[id].assigned < 0 ||
            !warm_skin_appearance(faces[id].color)) continue;
        const size_t slot = size_t(faces[id].assigned);
        if (skin_slots.size() <= slot) skin_slots.resize(slot + 1, 0);
        skin_slots[slot] = 1;
    }
    if (skin_slots.empty()) return;

    std::vector<uint8_t> shadow_candidate(count, 0), eligible(count, 0), blocked(count, 0);
    for (uint32_t id = 0; id < count; ++id) {
        shadow_candidate[id] = analysis.face_labels[id] == Label::FaceSkin &&
            analysis.face_confidence[id] >= minimum_ear_fold_confidence && faces[id].assigned < 0 &&
            faces[id].color[0] <= .62f && chroma(faces[id].color) <= .14f;
        eligible[id] = shadow_candidate[id] && analysis.face_confidence[id] >= minimum_confidence;
    }
    for (const auto& edge : surface_edges) for (const auto& side : {edge, std::make_pair(edge.second, edge.first)}) {
        if (!shadow_candidate[side.first] || analysis.face_confidence[side.second] < minimum_confidence) continue;
        const Label label = analysis.face_labels[side.second];
        if (face_detail_label(label)) blocked[side.first] = 1;
    }
    for (uint32_t id = 0; id < count; ++id)
        if (shadow_candidate[id] && point_detail_barrier[id]) blocked[id] = 1;
    // Generated portraits often keep the eyes on a separate shell, so neither
    // a shared edge nor an exact point contact is guaranteed. Protect dark
    // FaceSkin predictions in a small model-relative neighborhood around
    // reliable eye, eyebrow and mouth evidence. Ears remain far outside this
    // collar even when hair touches both regions in the rendered image.
    using Cell = std::array<long long, 3>;
    std::map<Cell, std::vector<uint32_t>> detail_cells;
    const auto spatial_detail = [](Label label) {
        return label == Label::Lips || label == Label::MouthInterior ||
            label == Label::EyeSclera || label == Label::Iris || label == Label::Eyebrow;
    };
    const auto cell = [&](const Vec3f& center) {
        Cell result {};
        for (int axis = 0; axis < 3; ++axis)
            result[axis] = static_cast<long long>(std::floor(center[axis] / detail_radius));
        return result;
    };
    if (detail_radius > 0 && std::isfinite(detail_radius)) {
        for (uint32_t id = 0; id < count; ++id)
            if (analysis.face_confidence[id] >= minimum_confidence && spatial_detail(analysis.face_labels[id]))
                detail_cells[cell(faces[id].center)].push_back(id);
        const float radius_squared = detail_radius * detail_radius;
        for (uint32_t id = 0; id < count; ++id) {
            if (!shadow_candidate[id] || blocked[id]) continue;
            const Cell origin = cell(faces[id].center);
            for (int z = -1; z <= 1 && !blocked[id]; ++z)
                for (int y = -1; y <= 1 && !blocked[id]; ++y)
                    for (int x = -1; x <= 1 && !blocked[id]; ++x) {
                        const auto found = detail_cells.find({origin[0] + x, origin[1] + y, origin[2] + z});
                        if (found == detail_cells.end()) continue;
                        for (uint32_t detail : found->second)
                            if ((faces[id].center - faces[detail].center).squaredNorm() <= radius_squared) {
                                blocked[id] = 1;
                                break;
                            }
                    }
        }
    }
    // A face-detail label usually occupies only the innermost eye or mouth
    // triangles. Keep the surrounding dark contour as a material boundary as
    // well; otherwise a chain of FaceSkin predictions can repaint eyelids,
    // eyebrows or the lip rim with the skin filament.
    for (uint8_t step = 0; step < maximum_steps; ++step) {
        std::vector<uint8_t> next = blocked;
        for (const auto& edge : edges) {
            if (blocked[edge.first] && shadow_candidate[edge.second]) next[edge.second] = 1;
            if (blocked[edge.second] && shadow_candidate[edge.first]) next[edge.first] = 1;
        }
        blocked.swap(next);
    }
    for (uint8_t step = 0; step < maximum_steps; ++step) {
        std::vector<std::array<uint8_t, 6>> votes(count);
        const auto visit = [&](uint32_t id, uint32_t neighbor) {
            if (!eligible[id] || blocked[id] || refined[id] >= 0 ||
                analysis.face_labels[neighbor] != Label::FaceSkin ||
                analysis.face_confidence[neighbor] < minimum_confidence) return;
            const int slot = refined[neighbor] >= 0 ? refined[neighbor] : faces[neighbor].assigned;
            if (slot < 0 || size_t(slot) >= skin_slots.size() || !skin_slots[size_t(slot)]) return;
            ++votes[id][size_t(slot)];
        };
        for (const auto& edge : edges) {
            visit(edge.first, edge.second);
            visit(edge.second, edge.first);
        }
        std::vector<std::pair<uint32_t, int>> proposed;
        for (uint32_t id = 0; id < count; ++id) {
            const auto best = std::max_element(votes[id].begin(), votes[id].end());
            if (*best >= 2) proposed.emplace_back(id, int(best - votes[id].begin()));
        }
        if (proposed.empty()) break;
        for (const auto& item : proposed) refined[item.first] = item.second;
    }

    // Ear folds may be split from both cheek skin and adjacent hair while still
    // receiving a high-confidence FaceSkin vote. Use frozen spatial support for
    // that specific ear/hair context; this pass cannot cascade.
    const float skin_radius = detail_radius / 6.f;
    const float hair_radius = detail_radius * (2.f / 3.f);
    if (skin_radius > 0 && hair_radius > 0 &&
        std::isfinite(skin_radius) && std::isfinite(hair_radius)) {
        using Cell = std::array<long long, 3>;
        const auto build_cells = [&](float radius, const auto& accept) {
            std::map<Cell, std::vector<uint32_t>> cells;
            for (uint32_t id = 0; id < count; ++id) if (accept(id)) {
                Cell key {};
                for (int axis = 0; axis < 3; ++axis)
                    key[axis] = static_cast<long long>(std::floor(faces[id].center[axis] / radius));
                cells[key].push_back(id);
            }
            return cells;
        };
        const auto skin_cells = build_cells(skin_radius, [&](uint32_t id) {
            const int slot = refined[id] >= 0 ? refined[id] : faces[id].assigned;
            return analysis.face_labels[id] == Label::FaceSkin &&
                analysis.face_confidence[id] >= minimum_confidence && slot >= 0 &&
                size_t(slot) < skin_slots.size() && skin_slots[size_t(slot)];
        });
        const auto hair_cells = build_cells(hair_radius, [&](uint32_t id) {
            return analysis.face_labels[id] == Label::Hair &&
                analysis.face_confidence[id] >= minimum_confidence && faces[id].assigned >= 0;
        });
        const auto nearby = [&](uint32_t id, float radius,
                                 const std::map<Cell, std::vector<uint32_t>>& cells,
                                 float minimum_normal_dot, bool absolute_normal = false) {
            Cell origin {};
            for (int axis = 0; axis < 3; ++axis)
                origin[axis] = static_cast<long long>(std::floor(faces[id].center[axis] / radius));
            const float radius_squared = radius * radius;
            uint32_t best = absent;
            float best_distance = radius_squared;
            for (int z = -1; z <= 1; ++z)
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x) {
                        const auto found = cells.find({origin[0] + x, origin[1] + y, origin[2] + z});
                        if (found == cells.end()) continue;
                        for (uint32_t candidate : found->second) {
                            const float normal_dot = faces[id].normal.dot(faces[candidate].normal);
                            if ((absolute_normal ? std::abs(normal_dot) : normal_dot) < minimum_normal_dot) continue;
                            const float distance = (faces[id].center - faces[candidate].center).squaredNorm();
                            if (distance < best_distance || (distance == best_distance && candidate < best)) {
                                best = candidate; best_distance = distance;
                            }
                        }
                    }
            return best;
        };
        std::vector<std::pair<uint32_t, int>> proposed;
        for (uint32_t id = 0; id < count; ++id) {
            if (!eligible[id] || blocked[id] || refined[id] >= 0 ||
                analysis.face_confidence[id] < .86f) continue;
            const uint32_t donor = nearby(id, skin_radius, skin_cells, .50f);
            if (donor == absent || nearby(id, hair_radius, hair_cells, .50f) == absent) continue;
            const int slot = refined[donor] >= 0 ? refined[donor] : faces[donor].assigned;
            proposed.emplace_back(id, slot);
        }
        for (const auto& item : proposed) refined[item.first] = item.second;

        // A low-confidence ear-rim face is not eligible for ordinary skin-hole
        // diffusion. Recover it only when frozen reliable skin and hair sources
        // independently establish the local ear/hair context. This pass is
        // intentionally one-shot and cannot turn its own output into support.
        const float uncertain_support_radius = detail_radius * (7.f / 24.f);
        const float uncertain_skin_radius = detail_radius * (7.f / 8.f);
        const float uncertain_hair_radius = detail_radius * (25.f / 12.f);
        const auto uncertain_support_cells = build_cells(uncertain_support_radius, [&](uint32_t id) {
            return analysis.face_labels[id] == Label::FaceSkin &&
                analysis.face_confidence[id] >= minimum_confidence;
        });
        const auto uncertain_skin_cells = build_cells(uncertain_skin_radius, [&](uint32_t id) {
            const int slot = refined[id] >= 0 ? refined[id] : faces[id].assigned;
            return analysis.face_labels[id] == Label::FaceSkin &&
                analysis.face_confidence[id] >= minimum_confidence && slot >= 0 &&
                size_t(slot) < skin_slots.size() && skin_slots[size_t(slot)];
        });
        const auto uncertain_hair_cells = build_cells(uncertain_hair_radius, [&](uint32_t id) {
            return analysis.face_labels[id] == Label::Hair &&
                analysis.face_confidence[id] >= minimum_confidence && faces[id].assigned >= 0;
        });
        std::vector<std::pair<uint32_t, int>> uncertain_proposed;
        for (uint32_t id = 0; id < count; ++id) {
            if (!shadow_candidate[id] || eligible[id] || blocked[id] || refined[id] >= 0) continue;
            const uint32_t support = nearby(id, uncertain_support_radius, uncertain_support_cells, .35f);
            const uint32_t donor = nearby(id, uncertain_skin_radius, uncertain_skin_cells, .35f, true);
            if (support == absent || donor == absent ||
                nearby(id, uncertain_hair_radius, uncertain_hair_cells, .35f) == absent ||
                appearance_distance(faces[id].color, faces[support].color) > .0025f ||
                appearance_distance(faces[id].color, faces[donor].color) > .0025f) continue;
            const int slot = refined[donor] >= 0 ? refined[donor] : faces[donor].assigned;
            uncertain_proposed.emplace_back(id, slot);
        }
        for (const auto& item : uncertain_proposed) refined[item.first] = item.second;
    }
}
struct SurfaceGraph {
    struct Neighbor { uint32_t face; float length; };
    std::vector<size_t> begin;
    std::vector<Neighbor> neighbors;
    SurfaceGraph(const std::vector<Face>& faces,
                 const std::vector<std::pair<uint32_t,uint32_t>>& edges) : begin(faces.size()+1,0),neighbors(edges.size()*2) {
        for (const auto& edge : edges) { ++begin[edge.first+1];++begin[edge.second+1]; }
        std::partial_sum(begin.begin(),begin.end(),begin.begin());
        auto cursor=begin;
        for (const auto& edge : edges) {
            const float length=(faces[edge.first].center-faces[edge.second].center).norm();
            neighbors[cursor[edge.first]++]={edge.second,length};
            neighbors[cursor[edge.second]++]={edge.first,length};
        }
    }
};
struct SurfaceReference {
    float distance {std::numeric_limits<float>::infinity()};
    uint32_t face {absent};
};
float chromaticity_distance(const Color&,const Color&);
std::vector<SurfaceReference> nearby_material_sources(
    const Analysis& analysis, const std::vector<Face>& faces, const std::vector<Color>& palette,
    const SurfaceGraph& graph, Label material, float radius, bool context_only=false)
{
    std::vector<SurfaceReference> result(faces.size());
    const auto reliable_clothes=[&](uint32_t id) {
        return analysis.face_labels[id]==Label::Clothes && analysis.face_confidence[id]>=minimum_confidence;
    };
    const auto allowed=[&](uint32_t id) {
        return faces[id].allowed && (context_only || (material==Label::Hair ?
            !reliable_clothes(id) && faces[id].color[0]<.78f : faces[id].bright));
    };
    for (uint32_t id=0;id<faces.size();++id) {
        const auto& face=faces[id];
        if (!allowed(id) || analysis.face_labels[id]!=material ||
            analysis.face_confidence[id]<minimum_confidence || face.assigned<0) continue;
        if (material==Label::Hair && face.color[0]>=.78f) continue;
        if (material==Label::Clothes && (face.color[0]<.78f || palette[size_t(face.assigned)][0]<.8f)) continue;
        result[id]={0.f,id};
    }
    using Visit=std::tuple<float,uint32_t,uint32_t>; // distance, source, face; ties are deterministic
    std::priority_queue<Visit,std::vector<Visit>,std::greater<Visit>> pending;
    const auto visit=[&](uint32_t id,uint32_t source,float distance) {
        if (!allowed(id) || distance>radius || distance>result[id].distance ||
            (distance==result[id].distance && source>=result[id].face)) return;
        result[id]={distance,source};pending.emplace(distance,source,id);
    };
    // Seed only the frontier. Hundreds of thousands of interior white/hair
    // source faces need not occupy the priority queue.
    for (uint32_t id=0;id<faces.size();++id) if (result[id].face==id)
        for (size_t i=graph.begin[id];i<graph.begin[id+1];++i) {
            if (material==Label::Clothes && chromaticity_distance(faces[id].color,faces[graph.neighbors[i].face].color)>.02f) continue;
            visit(graph.neighbors[i].face,id,graph.neighbors[i].length);
        }
    while (!pending.empty()) {
        const auto [distance,source,id]=pending.top();pending.pop();
        if (distance!=result[id].distance || source!=result[id].face) continue;
        for (size_t i=graph.begin[id];i<graph.begin[id+1];++i) {
            const auto& neighbor=graph.neighbors[i];
            if (material==Label::Clothes && chromaticity_distance(faces[id].color,faces[neighbor.face].color)>.02f) continue;
            visit(neighbor.face,source,distance+neighbor.length);
        }
    }
    return result;
}
float chromaticity_distance(const Color& a,const Color& b)
{
    if (a[0]<=0 || b[0]<=0) return std::numeric_limits<float>::infinity();
    return std::hypot(a[1]/a[0]-b[1]/b[0],a[2]/a[0]-b[2]/b[0]);
}
void protect_uncertain_contours(const Analysis& analysis, float radius,
                               const std::vector<std::pair<uint32_t,uint32_t>>& surface_edges,
                               std::vector<Face>& faces)
{
    // Source-neutral skin shadows can be Unknown next to a protected face.
    // Preserve a narrow surface-distance collar instead of letting a large
    // white clothing component absorb them. Traversal never enters accepted
    // hair/clothes, and never crosses a disconnected/occluded surface.
    struct Walk { uint32_t from,to; float distance; };
    std::vector<Walk> walks;
    const auto uncertain=[&](uint32_t id) {
        return analysis.face_labels[id]==Label::Unknown || analysis.face_labels[id]==Label::Background;
    };
    for (const auto& edge : surface_edges) {
        const bool ua=uncertain(edge.first),ub=uncertain(edge.second);
        if ((!ua && material_label(analysis.face_labels[edge.first])) ||
            (!ub && material_label(analysis.face_labels[edge.second])) || (!ua && !ub)) continue;
        const float distance=(faces[edge.first].center-faces[edge.second].center).norm();
        if (distance>radius) continue;
        walks.push_back({edge.first,edge.second,distance});walks.push_back({edge.second,edge.first,distance});
    }
    if (walks.empty()) return;
    std::sort(walks.begin(),walks.end(),[](const Walk& a,const Walk& b) { return a.from<b.from; });
    std::vector<size_t> begin(faces.size()+1,0);
    for (const auto& walk : walks) ++begin[walk.from+1];
    std::partial_sum(begin.begin(),begin.end(),begin.begin());
    std::vector<float> distances(faces.size(),std::numeric_limits<float>::infinity());
    using Visit=std::pair<float,uint32_t>;
    std::priority_queue<Visit,std::vector<Visit>,std::greater<Visit>> pending;
    for (const auto& walk : walks) if (!material_label(analysis.face_labels[walk.from]) && distances[walk.from]!=0.f) {
        distances[walk.from]=0;pending.emplace(0.f,walk.from);
    }
    while (!pending.empty()) {
        const auto current=pending.top();pending.pop();
        if (current.first!=distances[current.second]) continue;
        for (size_t i=begin[current.second];i<begin[current.second+1];++i) {
            const auto& walk=walks[i];const float proposed=current.first+walk.distance;
            if (proposed>radius || proposed>=distances[walk.to]) continue;
            distances[walk.to]=proposed;pending.emplace(proposed,walk.to);
        }
    }
    for (size_t id=0;id<faces.size();++id) if (uncertain(uint32_t(id)) && distances[id]<=radius)
        faces[id].allowed=faces[id].bright=faces[id].dark=false;
}
float narrow_width(const Region& region, const std::vector<Face>& faces)
{
    Vec3d mean = Vec3d::Zero();
    for (uint32_t id : region.faces) mean += faces[id].center.cast<double>() * faces[id].area;
    mean /= region.area;
    Eigen::Matrix3d covariance = Eigen::Matrix3d::Zero();
    for (uint32_t id : region.faces) {
        const Vec3d delta = faces[id].center.cast<double>() - mean;
        covariance += delta * delta.transpose() * faces[id].area;
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(covariance);
    if (solver.info() != Eigen::Success) return std::numeric_limits<float>::infinity();
    Vec3d lower = Vec3d::Constant(std::numeric_limits<double>::infinity()), upper = -lower;
    for (uint32_t id : region.faces) {
        const Vec3d position = solver.eigenvectors().transpose() * (faces[id].center.cast<double>() - mean);
        lower = lower.cwiseMin(position); upper = upper.cwiseMax(position);
    }
    std::array<double, 3> extent {upper[0]-lower[0], upper[1]-lower[1], upper[2]-lower[2]};
    std::sort(extent.begin(), extent.end());
    return float(extent[1]);
}
} // namespace

void refine_material_patches(const MeshSnapshot& source, const Analysis& analysis,
                             const std::vector<Color>& palette,
                             const std::vector<Color>& /*portrait_card*/, FaceColors& suggestions)
{
    const size_t count = source.mesh.indices.size(), vertex_count = source.mesh.vertices.size();
    if (!analysis.person_detected || analysis.canceled || !analysis.error.empty() ||
        source.geometry_id != analysis.geometry_id || source.content_id != analysis.content_id ||
        count == 0 || count > 2000000 || vertex_count == 0 || vertex_count > 6000000 ||
        palette.empty() || palette.size() > 6 || analysis.face_labels.size() != count ||
        analysis.face_confidence.size() != count) return;
    const bool vertex_colors = source.vertex_colors.size() == vertex_count;
    if (!vertex_colors && source.face_colors.size() != count) return;
    std::vector<Color> palette_labs;
    for (const auto& color : palette) { if (!valid_color(color)) return; palette_labs.push_back(to_lab(color)); }
    std::vector<Face> faces(count);
    for (const auto& assignment : suggestions) {
        const auto found = std::find(palette.begin(), palette.end(), assignment.second);
        if (assignment.first >= count || found == palette.end()) return;
        const int slot = int(found - palette.begin());
        if (faces[assignment.first].assigned >= 0 && faces[assignment.first].assigned != slot) return;
        faces[assignment.first].assigned = slot;
    }
    Vec3f lower = source.mesh.vertices.front(), upper = lower;
    for (const auto& vertex : source.mesh.vertices) {
        if (!vertex.allFinite()) return;
        lower = lower.cwiseMin(vertex); upper = upper.cwiseMax(vertex);
    }
    const float diagonal = (upper-lower).norm();
    if (diagonal <= 0 || !std::isfinite(diagonal)) return;
    double total_area = 0;
    bool have_hair = false, have_clothes = false, have_skin = false;
    for (size_t id = 0; id < count; ++id) {
        const auto& triangle = source.mesh.indices[id];
        const float confidence = analysis.face_confidence[id];
        if (size_t(analysis.face_labels[id]) >= label_count || !std::isfinite(confidence) || confidence < 0 || confidence > 1) return;
        Color rgb {};
        for (int corner = 0; corner < 3; ++corner) {
            const int vertex = triangle[corner]; if (vertex < 0 || size_t(vertex) >= vertex_count) return;
            const auto& rgba = vertex_colors ? source.vertex_colors[vertex] : source.face_colors[id];
            const Color color {rgba[0],rgba[1],rgba[2]}; if (!valid_color(color)) return;
            for (int channel = 0; channel < 3; ++channel) rgb[channel] += color[channel] / 3.f;
        }
        auto& face = faces[id]; face.color = to_lab(rgb);
        face.normal = (source.mesh.vertices[triangle[1]] - source.mesh.vertices[triangle[0]]).cross(
            source.mesh.vertices[triangle[2]] - source.mesh.vertices[triangle[0]]);
        const float length = face.normal.norm(); face.area = length * .5;
        total_area += face.area;
        if (length <= 0) continue;
        face.normal /= length;
        face.center = (source.mesh.vertices[triangle[0]] + source.mesh.vertices[triangle[1]] + source.mesh.vertices[triangle[2]]) / 3.f;
        face.allowed = material_label(analysis.face_labels[id]);
        face.bright = face.allowed && face.color[0] > .35f && chroma(face.color) < .04f;
        face.dark = face.allowed && face.color[0] <= .35f && chroma(face.color) < .12f;
        const bool assigned = face.assigned >= 0 && confidence >= minimum_confidence;
        have_hair |= assigned && face.dark && analysis.face_labels[id] == Label::Hair;
        have_clothes |= assigned && face.bright && analysis.face_labels[id] == Label::Clothes && face.color[0] >= .78f;
        have_skin |= assigned && analysis.face_labels[id] == Label::FaceSkin && warm_skin_appearance(face.color);
    }
    if (total_area <= 0 || (!have_hair && !have_clothes && !have_skin)) return;

    // Canonical IDs unify only exactly equal finite positions. No tolerance or
    // spatial bridge joins overlapping layers.
    std::vector<uint32_t> order(vertex_count), canonical(vertex_count);
    std::iota(order.begin(), order.end(), 0);
    const auto less_position = [&](uint32_t a, uint32_t b) {
        for (int axis = 0; axis < 3; ++axis) {
            if (source.mesh.vertices[a][axis] < source.mesh.vertices[b][axis]) return true;
            if (source.mesh.vertices[a][axis] > source.mesh.vertices[b][axis]) return false;
        }
        return a < b;
    };
    std::sort(order.begin(), order.end(), less_position);
    uint32_t previous = order.front(), canonical_id = previous;
    for (uint32_t vertex : order) {
        if (source.mesh.vertices[vertex] != source.mesh.vertices[previous]) canonical_id = vertex;
        canonical[vertex] = canonical_id; previous = vertex;
    }
    std::vector<Edge> edges; edges.reserve(count*3);
    std::vector<std::array<uint32_t,3>> triangles(count);
    for (uint32_t id = 0; id < count; ++id) {
        auto& triangle = triangles[id];
        for (int corner = 0; corner < 3; ++corner) triangle[corner] = canonical[source.mesh.indices[id][corner]];
        std::sort(triangle.begin(), triangle.end());
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t a = triangle[corner], b = triangle[(corner+1)%3];
            if (a != b) edges.push_back({(uint64_t(std::min(a,b)) << 32) | std::max(a,b), id});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& a,const Edge& b) { return a.key < b.key; });
    std::vector<std::pair<uint32_t,uint32_t>> neighbors;
    for (size_t first = 0; first < edges.size();) {
        size_t end = first+1; while (end < edges.size() && edges[end].key == edges[first].key) ++end;
        if (end-first == 2) {
            const uint32_t a = edges[first].face,b = edges[first+1].face;
            if (a != b && triangles[a] != triangles[b] &&
                faces[a].normal.dot(faces[b].normal) >= .75f) neighbors.emplace_back(a,b);
        }
        first = end;
    }
    std::vector<Edge>().swap(edges);
    // Dense generated portrait meshes may meet at an exact welded vertex while
    // using separate triangle edges. That is sufficient for a small, reliable
    // skin-shadow hole, but not for general material propagation. Duplicate
    // triangles, opposing sheets, and high-valence nonmanifold junctions are
    // excluded before these extra pairs reach the skin-only repair.
    struct VertexFace { uint32_t vertex; uint32_t face; };
    std::vector<VertexFace> vertex_faces;
    vertex_faces.reserve(count * 3);
    for (uint32_t id = 0; id < count; ++id) {
        if (analysis.face_confidence[id] < minimum_confidence ||
            (analysis.face_labels[id] != Label::FaceSkin &&
             !face_detail_label(analysis.face_labels[id]))) continue;
        for (uint32_t vertex : triangles[id]) vertex_faces.push_back({vertex, id});
    }
    std::sort(vertex_faces.begin(), vertex_faces.end(), [](const VertexFace& lhs, const VertexFace& rhs) {
        return lhs.vertex != rhs.vertex ? lhs.vertex < rhs.vertex : lhs.face < rhs.face;
    });
    // Only ear/hair boundary repair may cross a same-position vertex seam.
    // Such seams also occur densely around eyes, where treating point contact
    // as surface adjacency repaints the eyelid. A short shared-edge collar
    // around reliable hair provides the missing semantic context.
    std::vector<uint8_t> hair_boundary_distance(count, 255);
    for (const auto& edge : neighbors) for (const auto& side : {edge, std::make_pair(edge.second, edge.first)}) {
        if (analysis.face_labels[side.first] == Label::FaceSkin &&
            analysis.face_confidence[side.first] >= minimum_confidence &&
            analysis.face_labels[side.second] == Label::Hair &&
            analysis.face_confidence[side.second] >= minimum_confidence &&
            faces[side.second].assigned >= 0)
            hair_boundary_distance[side.first] = 0;
    }
    for (uint8_t step = 0; step < 6; ++step) for (const auto& edge : neighbors) {
        const auto extend = [&](uint32_t from, uint32_t to) {
            if (hair_boundary_distance[from] != step || hair_boundary_distance[to] <= step + 1 ||
                analysis.face_labels[to] != Label::FaceSkin ||
                analysis.face_confidence[to] < minimum_confidence) return;
            hair_boundary_distance[to] = step + 1;
        };
        extend(edge.first, edge.second);
        extend(edge.second, edge.first);
    }
    std::vector<std::pair<uint32_t,uint32_t>> skin_neighbors = neighbors;
    std::vector<uint8_t> point_detail_barrier(count, 0);
    for (size_t first = 0; first < vertex_faces.size();) {
        size_t end = first + 1;
        while (end < vertex_faces.size() && vertex_faces[end].vertex == vertex_faces[first].vertex) ++end;
        if (end - first <= 32) for (size_t a = first; a < end; ++a) for (size_t b = a + 1; b < end; ++b) {
            const uint32_t lhs = vertex_faces[a].face, rhs = vertex_faces[b].face;
            if (lhs == rhs || triangles[lhs] == triangles[rhs] ||
                faces[lhs].normal.dot(faces[rhs].normal) < .75f) continue;
            const bool lhs_skin = analysis.face_labels[lhs] == Label::FaceSkin;
            const bool rhs_skin = analysis.face_labels[rhs] == Label::FaceSkin;
            if (lhs_skin && face_detail_label(analysis.face_labels[rhs])) point_detail_barrier[lhs] = 1;
            if (rhs_skin && face_detail_label(analysis.face_labels[lhs])) point_detail_barrier[rhs] = 1;
            if (!lhs_skin || !rhs_skin ||
                (hair_boundary_distance[lhs] == 255 && hair_boundary_distance[rhs] == 255)) continue;
            skin_neighbors.emplace_back(std::min(lhs, rhs), std::max(lhs, rhs));
        }
        first = end;
    }
    std::sort(skin_neighbors.begin(), skin_neighbors.end());
    skin_neighbors.erase(std::unique(skin_neighbors.begin(), skin_neighbors.end()), skin_neighbors.end());
    std::vector<VertexFace>().swap(vertex_faces);
    std::vector<int> refined(count,-1);
    if (have_skin) {
        fill_supported_skin_gaps(analysis,faces,neighbors,diagonal*.012f,refined);
        fill_isolated_assigned_skin_holes(analysis,faces,neighbors,refined);
        fill_reliable_skin_shadow_holes(analysis,faces,skin_neighbors,neighbors,point_detail_barrier,
                                        diagonal*.012f,refined);
    }
    protect_uncertain_contours(analysis,diagonal*.005f,neighbors,faces);
    neighbors.erase(std::remove_if(neighbors.begin(),neighbors.end(),[&](const auto& edge) {
        return !faces[edge.first].allowed || !faces[edge.second].allowed;
    }),neighbors.end());
    const SurfaceGraph graph(faces,neighbors);
    const auto local_hair=nearby_material_sources(analysis,faces,palette_labs,graph,Label::Hair,diagonal*.005f);
    const auto local_white=nearby_material_sources(analysis,faces,palette_labs,graph,Label::Clothes,diagonal*.005f);
    // Material borrowing cannot cross reliable clothing. Junction risk must
    // also see hair beyond that clothing: this wider context can only veto a
    // shadow exemption, and is never used to assign a hair color.
    const auto hair_context=nearby_material_sources(analysis,faces,palette_labs,graph,Label::Hair,diagonal*.025f,true);
    DisjointSet bright_set(count), dark_set(count);
    for (const auto& edge : neighbors) {
        if (faces[edge.first].bright && faces[edge.second].bright) bright_set.join(edge.first,edge.second);
        if (faces[edge.first].dark && faces[edge.second].dark) dark_set.join(edge.first,edge.second);
    }
    std::map<uint32_t,Region> bright_regions;
    std::vector<double> dark_area(count,0);
    std::map<uint32_t,std::vector<double>> dark_votes;
    for (uint32_t id = 0; id < count; ++id) {
        if (faces[id].bright) { auto& region=bright_regions[bright_set.root(id)]; region.faces.push_back(id); region.area+=faces[id].area; }
        if (!faces[id].dark) continue;
        const uint32_t component=dark_set.root(id); dark_area[component]+=faces[id].area;
        if (analysis.face_labels[id] != Label::Hair || analysis.face_confidence[id] < minimum_confidence || faces[id].assigned < 0) continue;
        const auto& target=palette_labs[size_t(faces[id].assigned)];
        if (target[0] > .5f || chroma(target) > .08f) continue;
        auto& votes=dark_votes[component]; if (votes.empty()) votes.resize(palette.size(),0);
        votes[size_t(faces[id].assigned)] += faces[id].area;
    }
    std::map<uint32_t,std::vector<uint32_t>> boundaries;
    std::map<uint32_t,std::vector<std::pair<uint32_t,uint32_t>>> internal_edges;
    for (const auto& edge : neighbors)
        if (faces[edge.first].bright && faces[edge.second].bright)
            internal_edges[bright_set.root(edge.first)].push_back(edge);
    for (const auto& edge : neighbors) for (const auto& side : {edge,std::make_pair(edge.second,edge.first)})
        if (faces[side.first].bright && !faces[side.second].bright) boundaries[bright_set.root(side.first)].push_back(side.second);
    for (const auto& item : bright_regions) {
        const auto& region=item.second;
        std::vector<double> white_votes(palette.size(),0);
        double source_white=0,reliable_clothes=0,white_lightness=0;
        std::array<double,2> white_chromaticity {};
        for (uint32_t id : region.faces) {
            const auto& face=faces[id];
            if (analysis.face_labels[id] != Label::Clothes || analysis.face_confidence[id] < minimum_confidence) continue;
            reliable_clothes+=face.area;
            if (face.color[0] < .78f || face.assigned < 0 || palette_labs[size_t(face.assigned)][0] < .8f) continue;
            source_white+=face.area; white_lightness+=face.color[0]*face.area;
            for (size_t channel=0;channel<2;++channel)
                white_chromaticity[channel]+=face.color[channel+1]/face.color[0]*face.area;
            white_votes[size_t(face.assigned)]+=face.area;
        }
        const size_t white_slot=size_t(std::max_element(white_votes.begin(),white_votes.end())-white_votes.begin());
        const bool white_material=source_white >= region.area*.45 && white_votes[white_slot] >= source_white*.8 &&
            reliable_clothes >= region.area*.5;
        if (white_material) {
            const float lightness=float(white_lightness/source_white);
            for (double& channel : white_chromaticity) channel/=source_white;
            // A supported gray stripe has a sharp material boundary. A soft
            // source shadow has a continuous brightness transition into white.
            DisjointSet shade_set(region.faces.size());
            std::map<uint32_t,uint32_t> local;
            for (uint32_t i=0;i<region.faces.size();++i) local[region.faces[i]]=i;
            std::vector<uint8_t> shaded(region.faces.size(),0);
            for (uint32_t i=0;i<region.faces.size();++i) shaded[i]=faces[region.faces[i]].color[0] < lightness-.12f;
            const auto& region_edges=internal_edges[item.first];
            for (const auto& edge : region_edges) {
                const auto a=local.find(edge.first),b=local.find(edge.second);
                if (a!=local.end() && b!=local.end() && shaded[a->second] && shaded[b->second]) shade_set.join(a->second,b->second);
            }
            std::vector<double> shade_area(region.faces.size(),0),shade_clothes(region.faces.size(),0);
            std::vector<double> local_white_area(region.faces.size(),0);
            std::vector<float> shade_min(region.faces.size(),1.f),shade_max(region.faces.size(),0.f);
            std::vector<uint8_t> local_white_compatible(region.faces.size(),0),warm_shadow(region.faces.size(),0);
            std::vector<uint8_t> hair_junction(region.faces.size(),0);
            std::vector<size_t> sharp(region.faces.size(),0),perimeter(region.faces.size(),0);
            for (uint32_t i=0;i<region.faces.size();++i) {
                const uint32_t id=region.faces[i];
                if (shaded[i]) {
                    const uint32_t group=shade_set.root(i);const auto& color=faces[id].color;
                    shade_area[group]+=faces[id].area;
                    hair_junction[group]|=hair_context[id].face!=absent;
                    shade_min[group]=std::min(shade_min[group],color[0]);shade_max[group]=std::max(shade_max[group],color[0]);
                    const auto& white=local_white[id];const auto& hair=local_hair[id];
                    local_white_compatible[i]=white.face!=absent && white.distance<hair.distance &&
                        chromaticity_distance(color,faces[white.face].color)<=.02f;
                    if (local_white_compatible[i]) local_white_area[group]+=faces[id].area;
                    const bool locally_warm=std::hypot(color[1]/color[0]-white_chromaticity[0],
                                                      color[2]/color[0]-white_chromaticity[1])>.02;
                    warm_shadow[group]|=local_white_compatible[i] && locally_warm;
                }
                if (shaded[i] && analysis.face_labels[id]==Label::Clothes &&
                    analysis.face_confidence[id]>=minimum_confidence)
                    shade_clothes[shade_set.root(i)]+=faces[id].area;
            }
            for (const auto& edge : region_edges) for (const auto& side : {edge,std::make_pair(edge.second,edge.first)}) {
                const auto a=local.find(side.first),b=local.find(side.second);
                if (a==local.end() || b==local.end() || !shaded[a->second] || shaded[b->second]) continue;
                const uint32_t group=shade_set.root(a->second); ++perimeter[group];
                if (faces[side.second].color[0]-faces[side.first].color[0] >= .05f) ++sharp[group];
            }
            for (uint32_t i=0;i<region.faces.size();++i) {
                const uint32_t id=region.faces[i],group=shade_set.root(i);
                if (analysis.face_labels[id] == Label::Hair && analysis.face_confidence[id] >= minimum_confidence) continue;
                // A varying warm shadow can have a sharp luminance edge while
                // keeping the same local white-cloth chromaticity. This needs
                // nearby reliable sources on chromatically continuous paths
                // for almost the whole block. A pale edge or one warm face
                // must not erase a shaded dyed material. Varying brightness
                // alone must not erase a neutral gray stripe.
                const bool supported_warm_shadow=warm_shadow[group] && !hair_junction[group] && local_white_compatible[i] &&
                    local_white_area[group]>=shade_area[group]*.95 &&
                    shade_clothes[group]>=shade_area[group]*.1 && shade_max[group]-shade_min[group]>.12f;
                // An independent material boundary does not become a shadow
                // merely because a distant white garment makes its area small.
                // Require local clothing support; one isolated confident
                // triangle must not turn an otherwise uncertain shadow into
                // an independent cloth material. This denominator is only
                // the dark patch, independent of the outer garment's size.
                if (shaded[i] && !supported_warm_shadow && shade_clothes[group]>=shade_area[group]*.1 &&
                    perimeter[group]>0 && sharp[group]*2>=perimeter[group]) continue;
                // Normalized chromaticity is stable only under approximately
                // neutral illumination. Warm shadows also change it, so tint
                // alone cannot establish a different material. Require local
                // reliable hair or cloth evidence on the connected surface.
                const auto& color=faces[id].color;
                const bool tinted=shaded[i] && std::hypot(color[1]/color[0]-white_chromaticity[0],
                                                        color[2]/color[0]-white_chromaticity[1])>.02;
                if (tinted) {
                    const auto& hair=local_hair[id];const auto& white=local_white[id];
                    if (hair.face!=absent && hair.distance<white.distance &&
                        chromaticity_distance(color,faces[hair.face].color)<=.03f) continue;
                    if (analysis.face_labels[id]==Label::Clothes && analysis.face_confidence[id]>=minimum_confidence &&
                        (white.face==absent || chromaticity_distance(color,faces[white.face].color)>.02f)) continue;
                }
                refined[id]=int(white_slot);
            }
            continue;
        }
        if (!have_hair || region.area > total_area*.002 || narrow_width(region,faces) > diagonal*.025f) continue;
        const auto boundary=boundaries.find(item.first); if (boundary==boundaries.end() || boundary->second.empty()) continue;
        size_t dark_edges=0,hair_edges=0,clothes_edges=0;
        double surrounding_area=0;
        std::map<uint32_t,bool> counted;
        std::vector<double> target_votes(palette.size(),0);
        for (uint32_t id : boundary->second) {
            if (!faces[id].dark) continue;
            ++dark_edges;
            if (analysis.face_confidence[id] >= minimum_confidence) {
                hair_edges+=analysis.face_labels[id]==Label::Hair;
                clothes_edges+=analysis.face_labels[id]==Label::Clothes;
            }
            const uint32_t group=dark_set.root(id);
            const auto votes=dark_votes.find(group);
            if (votes==dark_votes.end() || counted[group]) continue;
            counted[group]=true; surrounding_area+=dark_area[group];
            for (size_t slot=0;slot<palette.size();++slot) target_votes[slot]+=votes->second[slot];
        }
        double inner_hair=0,inner_clothes=0;
        for (uint32_t id : region.faces) if (analysis.face_confidence[id] >= minimum_confidence) {
            if (analysis.face_labels[id]==Label::Hair) inner_hair+=faces[id].area;
            if (analysis.face_labels[id]==Label::Clothes) inner_clothes+=faces[id].area;
        }
        const double votes=std::accumulate(target_votes.begin(),target_votes.end(),0.);
        const size_t slot=size_t(std::max_element(target_votes.begin(),target_votes.end())-target_votes.begin());
        if (dark_edges*5 < boundary->second.size()*4 || hair_edges < 3 || hair_edges*3 < clothes_edges*7 ||
            votes<=0 || target_votes[slot]<votes*.8 || region.area>surrounding_area*.025 ||
            inner_hair<=0 || inner_clothes>inner_hair*.15) continue;
        for (uint32_t id : region.faces) refined[id]=int(slot);
    }
    if (palette.size() <= 4) {
        std::vector<int> white_sources(count, -1);
        const auto neutral_cloth = [&](uint32_t id) {
            return analysis.face_labels[id] == Label::Clothes && faces[id].allowed &&
                faces[id].color[0] >= .65f && chroma(faces[id].color) < .018f;
        };
        for (uint32_t id = 0; id < count; ++id) {
            if (!neutral_cloth(id) || analysis.face_confidence[id] < minimum_confidence ||
                faces[id].assigned < 0) continue;
            const size_t slot = size_t(faces[id].assigned);
            if (palette_labs[slot][0] >= .8f && chroma(palette_labs[slot]) < .02f)
                white_sources[id] = int(slot);
        }
        // Use only the original reliable garment as a seed. A short, same-label
        // surface path may fill uncertain neutral cloth, but cannot jump across
        // skin, hair, a chromatic stripe, or an abrupt gray material edge.
        for (int step = 0; step < 3; ++step) {
            std::vector<int> proposed(count, -1);
            const auto visit = [&](uint32_t from, uint32_t to) {
                if (white_sources[from] < 0 || !neutral_cloth(to) ||
                    analysis.face_confidence[to] < .35f ||
                    analysis.face_confidence[to] >= minimum_confidence ||
                    faces[to].assigned >= 0 || refined[to] >= 0 ||
                    std::abs(faces[from].color[0] - faces[to].color[0]) > .12f ||
                    chromaticity_distance(faces[from].color, faces[to].color) > .02f) return;
                proposed[to] = white_sources[from];
            };
            for (const auto& edge : neighbors) {
                visit(edge.first, edge.second);
                visit(edge.second, edge.first);
            }
            bool changed = false;
            for (uint32_t id = 0; id < count; ++id) if (proposed[id] >= 0) {
                refined[id] = white_sources[id] = proposed[id]; changed = true;
            }
            if (!changed) break;
        }
    }
    if (std::none_of(refined.begin(),refined.end(),[](int value){return value>=0;})) return;
    // Construct the replacement only after validation and classification finish.
    // Existing automatic targets survive on every excluded/protected face.
    FaceColors result; result.reserve(std::max(suggestions.size(),count/2));
    for (size_t id=0;id<count;++id) {
        const int slot=refined[id]>=0?refined[id]:faces[id].assigned;
        if (slot>=0) result.emplace_back(id,palette[size_t(slot)]);
    }
    suggestions.swap(result);
}

} // namespace Slic3r::AI::SemanticColoring
