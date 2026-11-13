#pragma once
#include "FirstLayerFlowDigits.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace flf {
struct Bounds { float xmin,xmax,ymin,ymax,zmin,zmax; };
inline Bounds bounds(const Mesh& mesh) {
    if (mesh.vertices.empty()) throw std::invalid_argument("empty mesh");
    const auto &v=mesh.vertices.front();
    Bounds b{v.x,v.x,v.y,v.y,v.z,v.z};
    for (const auto &p:mesh.vertices) {
        if (!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(p.z))
            throw std::invalid_argument("non-finite vertex");
        b.xmin=std::min(b.xmin,p.x); b.xmax=std::max(b.xmax,p.x);
        b.ymin=std::min(b.ymin,p.y); b.ymax=std::max(b.ymax,p.y);
        b.zmin=std::min(b.zmin,p.z); b.zmax=std::max(b.zmax,p.z);
    }
    for (const auto &t:mesh.triangles)
        if (t.a>=mesh.vertices.size()||t.b>=mesh.vertices.size()||t.c>=mesh.vertices.size())
            throw std::invalid_argument("invalid triangle index");
    return b;
}
inline void place(Mesh &m,float x,float y) {
    if (!std::isfinite(x)||!std::isfinite(y)) throw std::invalid_argument("invalid placement");
    for(auto &v:m.vertices) {v.x+=x;v.y+=y;}
}
inline void validate_single_layer(const Mesh &m,float height=0.24f) {
    auto b=bounds(m);
    if (std::abs(b.zmin)>1e-5f || std::abs(b.zmax-height)>1e-5f)
        throw std::invalid_argument("label outside first-layer Z bounds");
}
} // namespace flf
