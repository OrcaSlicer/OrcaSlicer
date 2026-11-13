#pragma once
// First-layer flow: dependency-free, seven-segment numeric label mesh.
// Coordinates are local millimeters; Z is always within [0, 0.24].
// This is a geometry module, NOT yet wired to OrcaSlicer's ModelVolume API.
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace flf {
struct Vertex { float x,y,z; };
struct Triangle { uint32_t a,b,c; };
struct Mesh { std::vector<Vertex> vertices; std::vector<Triangle> triangles; };

inline void box(Mesh& m, float x0,float y0,float x1,float y1,
                float z0=0.0f,float z1=0.24f) {
    if (!(x1>x0 && y1>y0 && z1>z0)) throw std::invalid_argument("bad box");
    const uint32_t b=static_cast<uint32_t>(m.vertices.size());
    const std::array<Vertex,8> v = {{
        {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
        {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}
    }};
    m.vertices.insert(m.vertices.end(),v.begin(),v.end());
    const std::array<std::array<uint32_t,3>,12> f = {{
        {{0,2,1}},{{0,3,2}},{{4,5,6}},{{4,6,7}},
        {{0,1,5}},{{0,5,4}},{{1,2,6}},{{1,6,5}},
        {{2,3,7}},{{2,7,6}},{{3,0,4}},{{3,4,7}}
    }};
    for(auto t:f) m.triangles.push_back({b+t[0],b+t[1],b+t[2]});
}

// Bits: top, upper-right, lower-right, bottom, lower-left,
// upper-left, middle. 2.6 mm high, 2.6 mm wide; stroke 0.45 mm.
inline uint8_t digit_mask(char c) {
    switch(c) {
        case '0': return 0b0111111;
        case '1': return 0b0000110;
        case '2': return 0b1011011;
        case '3': return 0b1001111;
        case '4': return 0b1100110;
        case '5': return 0b1101101;
        case '6': return 0b1111101;
        case '7': return 0b0000111;
        case '8': return 0b1111111;
        case '9': return 0b1101111;
        default: throw std::invalid_argument("not a digit");
    }
}
inline Mesh label_mesh(const std::string& label) {
    if (label.empty() || label.size()>8) throw std::invalid_argument("label length");
    Mesh m;
    constexpr float w=2.6f,h=3.0f,s=0.45f,advance=3.2f;
    float x=0;
    for(char c:label) {
        if(c=='.') {
            box(m,x+0.1f,0.05f,x+0.65f,0.60f);
            x+=1.15f;
            continue;
        }
        const uint8_t bits=digit_mask(c);
        if(bits&1)  box(m,x+s,h-s,x+w-s,h);                 // top
        if(bits&2)  box(m,x+w-s,h/2+s/2,x+w,h-s);         // upper right
        if(bits&4)  box(m,x+w-s,s,x+w,h/2-s/2);           // lower right
        if(bits&8)  box(m,x+s,0,x+w-s,s);                 // bottom
        if(bits&16) box(m,x,s,x+s,h/2-s/2);               // lower left
        if(bits&32) box(m,x,h/2+s/2,x+s,h-s);             // upper left
        if(bits&64) box(m,x+s,h/2-s/2,x+w-s,h/2+s/2);    // middle
        x+=advance;
    }
    return m;
}
} // namespace flf
