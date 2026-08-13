#include "MixedFilament.hpp"
#include "ClipperUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace Slic3r {

namespace {

std::vector<std::string> split_char(const std::string &s, char delim)
{
    std::vector<std::string> parts;
    std::string              cur;
    for (char c : s) {
        if (c == delim) {
            parts.emplace_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.emplace_back(std::move(cur));
    return parts;
}

std::string trim_copy(const std::string &s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

int parse_int_default(const std::string &s, int def)
{
    if (s.empty())
        return def;
    char *end = nullptr;
    long  v   = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str())
        return def;
    return int(v);
}

float parse_float_default(const std::string &s, float def)
{
    if (s.empty())
        return def;
    char  *end = nullptr;
    float  v   = std::strtof(s.c_str(), &end);
    if (end == s.c_str())
        return def;
    return v;
}

bool parse_offset_token(const std::string &field, MixedFilament &mf)
{
    if (field.size() >= 2 && (field[0] == 'x' || field[0] == 'X') &&
        (field[1] == 'a' || field[1] == 'A')) {
        mf.component_a_surface_offset = parse_float_default(field.substr(2), 0.f);
        return true;
    }
    if (field.size() >= 2 && (field[0] == 'x' || field[0] == 'X') &&
        (field[1] == 'b' || field[1] == 'B')) {
        mf.component_b_surface_offset = parse_float_default(field.substr(2), 0.f);
        return true;
    }
    return false;
}

std::string format_offset_token(char which, float value)
{
    std::ostringstream oss;
    oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
    oss << 'x' << which << value;
    return oss.str();
}

unsigned int clamp_component(unsigned int id, size_t num_physical)
{
    if (num_physical == 0)
        return 1;
    if (id < 1)
        return 1;
    if (id > num_physical)
        return unsigned(num_physical);
    return id;
}

bool is_pattern_separator(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '-' || c == '_' || c == '|' ||
           c == ':' || c == ';' || c == ',';
}

// Map pattern token to physical filament ID (1-based).
// '1' → component_a, '2' → component_b, '3'..'9' → direct physical (clamped).
unsigned int physical_from_pattern_token(char token, const MixedFilament &mf, size_t num_physical)
{
    if (token == '1')
        return clamp_component(mf.component_a, num_physical);
    if (token == '2')
        return clamp_component(mf.component_b, num_physical);
    if (token >= '3' && token <= '9') {
        const unsigned int direct = unsigned(token - '0');
        return clamp_component(direct, num_physical);
    }
    return clamp_component(mf.component_a, num_physical);
}

int safe_mod(int value, int modulus)
{
    if (modulus <= 0)
        return 0;
    int r = value % modulus;
    if (r < 0)
        r += modulus;
    return r;
}

} // namespace

std::string MixedFilamentManager::normalize_manual_pattern(const std::string &pattern)
{
    // M4: simple whole-layer pattern. Drop separators; keep '1'..'9' only.
    // For comma-grouped patterns like "12,21", keep first group only (whole-layer resolve).
    std::string first_group;
    first_group.reserve(pattern.size());
    for (char c : pattern) {
        if (c == ',')
            break;
        if (is_pattern_separator(c))
            continue;
        if (c >= '1' && c <= '9')
            first_group.push_back(c);
        else if (c == 'a' || c == 'A')
            first_group.push_back('1');
        else if (c == 'b' || c == 'B')
            first_group.push_back('2');
    }
    return first_group;
}

size_t MixedFilamentManager::max_filament_id(const std::string &serialized, size_t num_physical)
{
    if (num_physical == 0)
        return 0;
    if (serialized.empty())
        return num_physical;
    MixedFilamentManager mgr;
    mgr.load_definitions(serialized);
    return mgr.total_filaments(num_physical);
}

void MixedFilamentManager::clear()
{
    m_mixed.clear();
}

void MixedFilamentManager::load_definitions(const std::string &serialized)
{
    m_mixed.clear();
    if (serialized.empty())
        return;

    for (const std::string &row_raw : split_char(serialized, ';')) {
        const std::string row = trim_copy(row_raw);
        if (row.empty())
            continue;

        const std::vector<std::string> fields = split_char(row, ',');
        if (fields.size() < 2)
            continue;

        MixedFilament mf;
        mf.component_a = unsigned(std::max(0, parse_int_default(trim_copy(fields[0]), 1)));
        mf.component_b = unsigned(std::max(0, parse_int_default(trim_copy(fields[1]), 2)));
        mf.enabled     = fields.size() > 2 ? (parse_int_default(trim_copy(fields[2]), 1) != 0) : true;
        mf.ratio_a     = fields.size() > 3 ? parse_int_default(trim_copy(fields[3]), 1) : 1;
        mf.ratio_b     = fields.size() > 4 ? parse_int_default(trim_copy(fields[4]), 1) : 1;
        for (size_t i = 5; i < fields.size(); ++i) {
            const std::string token = trim_copy(fields[i]);
            if (token.empty())
                continue;
            if (parse_offset_token(token, mf))
                continue;
            if (mf.manual_pattern.empty())
                mf.manual_pattern = normalize_manual_pattern(token);
        }

        if (mf.component_a == 0)
            mf.component_a = 1;
        if (mf.component_b == 0)
            mf.component_b = 2;
        if (mf.ratio_a < 0)
            mf.ratio_a = 0;
        if (mf.ratio_b < 0)
            mf.ratio_b = 0;

        // Keep enabled rows only.
        if (mf.enabled)
            m_mixed.emplace_back(mf);
    }
}

std::string MixedFilamentManager::serialize_definitions() const
{
    std::ostringstream oss;
    for (size_t i = 0; i < m_mixed.size(); ++i) {
        const MixedFilament &mf = m_mixed[i];
        if (i)
            oss << ';';
        oss << mf.component_a << ',' << mf.component_b << ','
            << (mf.enabled ? 1 : 0) << ',' << mf.ratio_a << ',' << mf.ratio_b;
        const std::string pattern = normalize_manual_pattern(mf.manual_pattern);
        if (!pattern.empty())
            oss << ',' << pattern;
        if (std::abs(mf.component_a_surface_offset) > 1e-6f)
            oss << ',' << format_offset_token('a', mf.component_a_surface_offset);
        if (std::abs(mf.component_b_surface_offset) > 1e-6f)
            oss << ',' << format_offset_token('b', mf.component_b_surface_offset);
    }
    return oss.str();
}

int MixedFilamentManager::mixed_index_from_filament_id(unsigned int filament_id_1based, size_t num_physical) const
{
    if (num_physical == 0 || filament_id_1based <= num_physical)
        return -1;
    const size_t idx = size_t(filament_id_1based - num_physical - 1);
    if (idx >= m_mixed.size())
        return -1;
    return int(idx);
}

bool MixedFilamentManager::is_mixed(unsigned int filament_id_1based, size_t num_physical) const
{
    return mixed_index_from_filament_id(filament_id_1based, num_physical) >= 0;
}

const MixedFilament *MixedFilamentManager::mixed_filament_from_id(unsigned int filament_id_1based, size_t num_physical) const
{
    const int idx = mixed_index_from_filament_id(filament_id_1based, num_physical);
    if (idx < 0)
        return nullptr;
    return &m_mixed[size_t(idx)];
}

float MixedFilamentManager::component_surface_offset(unsigned int filament_id_1based, size_t num_physical, int layer_index) const
{
    const MixedFilament *mf = mixed_filament_from_id(filament_id_1based, num_physical);
    if (mf == nullptr)
        return 0.f;
    const unsigned int physical = resolve(filament_id_1based, num_physical, layer_index);
    if (physical == clamp_component(mf->component_a, num_physical))
        return mf->component_a_surface_offset;
    return mf->component_b_surface_offset;
}

unsigned int MixedFilamentManager::resolve(unsigned int filament_id_1based, size_t num_physical, int layer_index) const
{
    const MixedFilament *mf = mixed_filament_from_id(filament_id_1based, num_physical);
    if (mf == nullptr)
        return filament_id_1based;

    // Pattern takes precedence when non-empty after normalize.
    // Adapted from FullSpectrum_integration MixedFilamentManager::resolve (pattern branch).
    const std::string pattern = normalize_manual_pattern(mf->manual_pattern);
    if (!pattern.empty()) {
        const int pos = safe_mod(layer_index, int(pattern.size()));
        return physical_from_pattern_token(pattern[size_t(pos)], *mf, num_physical);
    }

    int ratio_a = std::max(0, mf->ratio_a);
    int ratio_b = std::max(0, mf->ratio_b);
    if (ratio_a == 0 && ratio_b == 0) {
        return clamp_component(mf->component_a, num_physical);
    }

    const int cycle = std::max(1, ratio_a + ratio_b);
    const int pos   = safe_mod(layer_index, cycle);

    const unsigned int chosen = (pos < ratio_a) ? mf->component_a : mf->component_b;
    return clamp_component(chosen, num_physical);
}

void MixedFilamentManager::append_physical_0based(unsigned int filament_id_1based, size_t num_physical, std::vector<unsigned int> &out) const
{
    if (filament_id_1based == 0 || num_physical == 0)
        return;

    if (const MixedFilament *mf = mixed_filament_from_id(filament_id_1based, num_physical)) {
        const unsigned int a = clamp_component(mf->component_a, num_physical);
        const unsigned int b = clamp_component(mf->component_b, num_physical);
        out.emplace_back(a - 1);
        if (b != a)
            out.emplace_back(b - 1);
        return;
    }

    if (filament_id_1based >= 1 && filament_id_1based <= num_physical)
        out.emplace_back(filament_id_1based - 1);
    else if (filament_id_1based > num_physical)
        out.emplace_back(0); // unknown virtual → first physical (safe fallback)
}

ExPolygons apply_surface_offset(const ExPolygons &src, float offset_mm)
{
    if (src.empty() || std::abs(offset_mm) <= 1e-6f)
        return src;
    const float delta_scaled = float(scale_(std::abs(double(offset_mm))));
    if (delta_scaled <= float(EPSILON))
        return src;
    ExPolygons adjusted = offset_mm > 0.f ? offset_ex(src, -delta_scaled) : offset_ex(src, delta_scaled);
    if (!adjusted.empty() && adjusted.size() > 1)
        adjusted = union_ex(adjusted);
    return adjusted;
}

} // namespace Slic3r
