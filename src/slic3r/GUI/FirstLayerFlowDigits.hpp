#pragma once
// Printable, connected 7x9 bitmap numerals for first-layer flow labels.
// Each occupied pixel is 0.50 mm square. Adjacent pixels share one solid
// manifold mesh, with internal walls omitted. Z spans exactly 0..0.24 mm.
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace flf {
struct Vertex { float x, y, z; };
struct Triangle { uint32_t a, b, c; };
struct Mesh { std::vector<Vertex> vertices; std::vector<Triangle> triangles; };

// Seven columns by nine rows, top row first. Conventional, block sans-serif.
inline const std::array<const char*, 9>& digit_rows(char c) {
    static const std::array<const char*, 9> glyphs[] = {
        {{".#####.","##...##","##...##","##...##","##...##","##...##","##...##","##...##",".#####."}}, // 0
        {{"...##..","..###..","...##..","...##..","...##..","...##..","...##..","...##..",".######"}}, // 1
        {{".#####.","##...##",".....##",".....##","...###.",".###...","##.....","##.....","#######"}}, // 2
        {{".#####.","##...##",".....##",".....##","..####.",".....##",".....##","##...##",".#####."}}, // 3
        {{"....##.","...###.","..####.",".##.##.","##..##.","#######","....##.","....##.","....##."}}, // 4
        {{"#######","##.....","##.....","######.",".....##",".....##",".....##","##...##",".#####."}}, // 5
        {{".#####.","##...##","##.....","######.","##...##","##...##","##...##","##...##",".#####."}}, // 6
        {{"#######",".....##","....##.","....##.","...##..","...##..","..##...","..##...","..##..."}}, // 7
        {{".#####.","##...##","##...##","##...##",".#####.","##...##","##...##","##...##",".#####."}}, // 8
        {{".#####.","##...##","##...##","##...##",".######",".....##",".....##","##...##",".#####."}}  // 9
    };
    if (c < '0' || c > '9') throw std::invalid_argument("not a digit");
    return glyphs[c - '0'];
}

inline Mesh label_mesh(const std::string& label) {
    if (label.empty() || label.size() > 8)
        throw std::invalid_argument("label length");
    constexpr float cell = 0.50f;
    constexpr float height = 0.24f;
    constexpr int cols = 7, rows = 9;
    Mesh mesh;
    float x_cursor = 0.0f;

    for (char ch : label) {
        bool pixels[rows][cols] = {};
        int width = cols;
        if (ch == '.') {
            // 2x2 square dot, baseline aligned and connected within itself.
            pixels[7][0] = pixels[7][1] = true;
            pixels[8][0] = pixels[8][1] = true;
            width = 2;
        } else {
            const auto& glyph = digit_rows(ch);
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                    pixels[r][c] = glyph[r][c] == '#';
        }

        // Every cell corner is shared by neighboring occupied cells. The
        // shared grid keeps top/bottom faces and exposed side walls conforming.
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        for (int z = 0; z < 2; ++z)
            for (int r = 0; r <= rows; ++r)
                for (int c = 0; c <= width; ++c)
                    mesh.vertices.push_back({x_cursor + c * cell,
                                             (rows - r) * cell,
                                             z ? height : 0.0f});
        const uint32_t plane = (rows + 1) * (width + 1);
        auto v = [&](int r, int c, int z) -> uint32_t {
            return base + z * plane + r * (width + 1) + c;
        };
        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            mesh.triangles.push_back({a, b, c});
        };
        auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
            tri(a, b, c); tri(a, c, d);
        };
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < width; ++c) {
                if (!pixels[r][c]) continue;
                // Bottom (-Z), top (+Z).
                quad(v(r,c,0),v(r,c+1,0),v(r+1,c+1,0),v(r+1,c,0));
                quad(v(r,c,1),v(r+1,c,1),v(r+1,c+1,1),v(r,c+1,1));
                // Top edge (+Y), bottom edge (-Y), left (-X), right (+X).
                if (r == 0 || !pixels[r-1][c])
                    quad(v(r,c,0),v(r,c,1),v(r,c+1,1),v(r,c+1,0));
                if (r == rows-1 || !pixels[r+1][c])
                    quad(v(r+1,c,0),v(r+1,c+1,0),v(r+1,c+1,1),v(r+1,c,1));
                if (c == 0 || !pixels[r][c-1])
                    quad(v(r,c,0),v(r+1,c,0),v(r+1,c,1),v(r,c,1));
                if (c == width-1 || !pixels[r][c+1])
                    quad(v(r,c+1,0),v(r,c+1,1),v(r+1,c+1,1),v(r+1,c+1,0));
            }
        }
        x_cursor += (width + 1) * cell;
    }
    return mesh;
}
} // namespace flf
