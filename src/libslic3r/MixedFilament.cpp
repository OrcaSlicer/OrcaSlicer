#include "MixedFilament.hpp"

#include <algorithm>
#include <cctype>
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

} // namespace

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

        if (mf.component_a == 0)
            mf.component_a = 1;
        if (mf.component_b == 0)
            mf.component_b = 2;
        if (mf.ratio_a < 0)
            mf.ratio_a = 0;
        if (mf.ratio_b < 0)
            mf.ratio_b = 0;

        // M3: keep enabled rows only.
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

unsigned int MixedFilamentManager::resolve(unsigned int filament_id_1based, size_t num_physical, int layer_index) const
{
    const MixedFilament *mf = mixed_filament_from_id(filament_id_1based, num_physical);
    if (mf == nullptr)
        return filament_id_1based;

    int ratio_a = std::max(0, mf->ratio_a);
    int ratio_b = std::max(0, mf->ratio_b);
    if (ratio_a == 0 && ratio_b == 0) {
        return clamp_component(mf->component_a, num_physical);
    }

    const int cycle = std::max(1, ratio_a + ratio_b);
    int       pos   = layer_index % cycle;
    if (pos < 0)
        pos += cycle;

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

} // namespace Slic3r
