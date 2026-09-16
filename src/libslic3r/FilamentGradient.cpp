#include "FilamentGradient.hpp"
#include "Color.hpp"

#include <sstream>
#include <stdexcept>

namespace Slic3r {

std::string FilamentGradient::serialize_stops() const
{
    std::ostringstream ss;
    for (size_t i = 0; i < stops.size(); ++i) {
        if (i > 0) ss << ';';
        ss << stops[i].position << ',' << encode_color(stops[i].color);
    }
    return ss.str();
}

bool FilamentGradient::parse_stops(const std::string& s, std::vector<GradientColorStop>& out)
{
    out.clear();
    if (s.empty()) return true;

    std::istringstream ss(s);
    std::string token;
    while (std::getline(ss, token, ';')) {
        const auto comma = token.find(',');
        if (comma == std::string::npos) return false;

        GradientColorStop stop;
        try {
            stop.position = std::stof(token.substr(0, comma));
        } catch (...) { return false; }

        if (!decode_color(token.substr(comma + 1), stop.color))
            return false;

        out.push_back(stop);
    }

    std::sort(out.begin(), out.end());
    return out.size() >= 2;
}

bool FilamentGradient::stops_from_color_list(const std::string& s, std::vector<GradientColorStop>& out)
{
    out.clear();
    if (s.empty()) return false;

    // Split on whitespace or commas (native filament_multi_colour is space-separated).
    std::vector<ColorRGBA> colors;
    std::string token;
    std::istringstream ss(s);
    while (ss >> token) {
        // a token may itself contain commas
        size_t start = 0;
        while (start < token.size()) {
            const size_t comma = token.find(',', start);
            const std::string piece = token.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            ColorRGBA c;
            if (!piece.empty() && decode_color(piece, c))
                colors.push_back(c);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    if (colors.size() < 2) return false;

    const float denom = static_cast<float>(colors.size() - 1);
    out.reserve(colors.size());
    for (size_t i = 0; i < colors.size(); ++i)
        out.push_back({ static_cast<float>(i) / denom, colors[i] });
    return true;
}

} // namespace Slic3r
