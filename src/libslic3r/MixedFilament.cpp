#include "MixedFilament.hpp"
#include "ClipperUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <numeric>
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

// Named optional tokens after the pair prefix. Parse order is load-bearing:
// existing xa/xb, then rc before c (longer prefix first, case-insensitive like xa/xb),
// then pattern fallback. cN must be recognized before normalize_manual_pattern
// because normalize_manual_pattern("c3") strips 'c' and would set pattern "3".
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

bool parse_ratio_c_token(const std::string &field, MixedFilament &mf)
{
    if (field.size() >= 2 && (field[0] == 'r' || field[0] == 'R') &&
        (field[1] == 'c' || field[1] == 'C')) {
        mf.ratio_c = std::max(0, parse_int_default(field.substr(2), 0));
        return true;
    }
    return false;
}

bool parse_component_c_token(const std::string &field, MixedFilament &mf)
{
    if (field.size() >= 2 && (field[0] == 'c' || field[0] == 'C')) {
        const int v = parse_int_default(field.substr(1), -1);
        if (v < 0)
            return false;
        mf.component_c = unsigned(v);
        return true;
    }
    return false;
}

// Claim every g/G-prefix token so typos never fall through to pattern.
// Enable only bare g/G or g1/G1; consume-but-do-not-enable g0/g2/….
bool parse_gradient_token(const std::string &token, MixedFilament &mf)
{
    if (token.empty() || (token[0] != 'g' && token[0] != 'G'))
        return false;
    if (token.size() == 1) {
        mf.gradient_enabled = true;
        return true;
    }
    if (token.size() == 2 && token[1] == '1') {
        mf.gradient_enabled = true;
        return true;
    }
    // Consumed, not enabled, not pattern.
    return true;
}

// Claim every p/P-prefix token so typos never fall through to pattern.
// Enable only bare p/P or p1/P1; consume-but-do-not-enable p0/p2/….
bool parse_perimeter_token(const std::string &token, MixedFilament &mf)
{
    if (token.empty() || (token[0] != 'p' && token[0] != 'P'))
        return false;
    if (token.size() == 1) {
        mf.perimeter_modulation = true;
        return true;
    }
    if (token.size() == 2 && token[1] == '1') {
        mf.perimeter_modulation = true;
        return true;
    }
    // Consumed, not enabled, not pattern.
    return true;
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
// '1' → component_a, '2' → component_b,
// '3' → component_c when set else physical 3, '4'..'9' → direct physical (clamped).
unsigned int physical_from_pattern_token(char token, const MixedFilament &mf, size_t num_physical)
{
    if (token == '1')
        return clamp_component(mf.component_a, num_physical);
    if (token == '2')
        return clamp_component(mf.component_b, num_physical);
    if (token == '3' && mf.component_c != 0)
        return clamp_component(mf.component_c, num_physical);
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

bool parse_int_full(const std::string &s, int &out)
{
    const std::string t = trim_copy(s);
    if (t.empty())
        return false;
    char *end = nullptr;
    long  v   = std::strtol(t.c_str(), &end, 10);
    if (end == t.c_str() || *end != '\0')
        return false;
    out = int(v);
    return true;
}

bool is_prefixed_int_token(const std::string &tok, char lower)
{
    if (tok.size() < 2)
        return false;
    const char c = tok[0];
    if (c != lower && c != char(std::toupper(static_cast<unsigned char>(lower))))
        return false;
    for (size_t i = 1; i < tok.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(tok[i])))
            return false;
    }
    return true;
}

// FullSpec 3mf recipes use Snapmaker custom-entry grammar (uN / mN / oN).
// Translate on load to ZR pair+pattern rows; persist remains ZR-only
// (no mix_b_percent, gradient_component_ids, or uN).
bool parse_snapmaker_row_to_zr(const std::string &row, MixedFilament &out)
{
    std::vector<std::string> tokens;
    for (const std::string &raw : split_char(row, ','))
        tokens.push_back(trim_copy(raw));
    if (tokens.size() < 5)
        return false;

    int values[5] = {0, 0, 1, 1, 50};
    for (size_t i = 0; i < 5; ++i) {
        if (!parse_int_full(tokens[i], values[i]))
            return false;
    }
    if (values[0] <= 0 || values[1] <= 0)
        return false;

    const bool enabled = values[2] != 0;
    int        mix_b   = values[4];
    if (mix_b < 0)
        mix_b = 0;
    if (mix_b > 100)
        mix_b = 100;

    bool                     deleted = false;
    std::string              gradient_ids;
    std::vector<std::string> pattern_tokens;

    size_t token_idx = 5;
    if (tokens.size() >= 6) {
        // Snapmaker v2.3.5 parse_row_definition token[5] branch:
        // "0"/"1" = pointillism (consume, drop); empty/g/m/r = metadata start; else pattern.
        const std::string &legacy = tokens[5];
        if (legacy == "0" || legacy == "1") {
            token_idx = 6;
        } else if (legacy.empty() || legacy[0] == 'g' || legacy[0] == 'G' ||
                   legacy[0] == 'm' || legacy[0] == 'M' ||
                   legacy[0] == 'r' || legacy[0] == 'R') {
            token_idx = 5;
        } else {
            pattern_tokens.push_back(legacy);
            token_idx = 6;
        }
    }

    for (size_t i = token_idx; i < tokens.size(); ++i) {
        const std::string &tok = tokens[i];
        if (tok.empty())
            continue;
        const char c0 = tok[0];
        if (c0 == 'g' || c0 == 'G') {
            gradient_ids = tok.substr(1);
            continue;
        }
        if (c0 == 'w' || c0 == 'W')
            continue;
        if (c0 == 'm' || c0 == 'M')
            continue;
        if (c0 == 'z' || c0 == 'Z')
            continue;
        if ((c0 == 'x' || c0 == 'X') && tok.size() >= 2) {
            const char component = char(std::tolower(static_cast<unsigned char>(tok[1])));
            if (component == 'a' || component == 'b')
                continue;
        }
        if (c0 == 'd' || c0 == 'D') {
            int parsed = 0;
            if (parse_int_full(tok.substr(1), parsed))
                deleted = parsed != 0;
            continue;
        }
        if (c0 == 'o' || c0 == 'O')
            continue;
        if (c0 == 'u' || c0 == 'U')
            continue;
        if ((c0 == 'c' || c0 == 'C') && tok.size() >= 3 && (tok[1] == 'm' || tok[1] == 'M'))
            continue;
        if (c0 == 'r' || c0 == 'R')
            continue;
        pattern_tokens.push_back(tok);
    }

    if (!enabled || deleted)
        return false;

    MixedFilament mf;
    mf.component_a      = unsigned(values[0]);
    mf.component_b      = unsigned(values[1]);
    mf.enabled          = true;
    mf.gradient_enabled = false;

    if (!pattern_tokens.empty()) {
        std::ostringstream joined;
        for (size_t i = 0; i < pattern_tokens.size(); ++i) {
            if (i)
                joined << ',';
            joined << pattern_tokens[i];
        }
        mf.manual_pattern = MixedFilamentManager::normalize_manual_pattern(joined.str());
    }

    // SHOULD: non-empty g{ids} with no pattern → single-digit ids 1–9 as a pattern.
    // Decline 1% weights / BlendLUT.
    if (mf.manual_pattern.empty() && !gradient_ids.empty()) {
        std::string from_g;
        from_g.reserve(gradient_ids.size());
        for (char ch : gradient_ids) {
            if (ch >= '1' && ch <= '9')
                from_g.push_back(ch);
        }
        mf.manual_pattern = MixedFilamentManager::normalize_manual_pattern(from_g);
    }

    if (!mf.manual_pattern.empty()) {
        mf.ratio_a = 1;
        mf.ratio_b = 1;
    } else {
        int       ra = 100 - mix_b;
        int       rb = mix_b;
        const int g  = int(std::gcd(unsigned(std::abs(ra)), unsigned(std::abs(rb))));
        const int d  = g == 0 ? 1 : g;
        ra /= d;
        rb /= d;
        if (ra == 0 && rb == 0) {
            ra = 1;
            rb = 1;
        }
        mf.ratio_a = ra;
        mf.ratio_b = rb;
    }

    out = mf;
    return true;
}

std::string translate_snapmaker_custom_entries_to_zr(const std::string &serialized)
{
    std::ostringstream oss;
    bool               first = true;
    for (const std::string &row_raw : split_char(serialized, ';')) {
        const std::string row = trim_copy(row_raw);
        if (row.empty())
            continue;
        MixedFilament mf;
        if (!parse_snapmaker_row_to_zr(row, mf))
            continue;
        if (first)
            first = false;
        else
            oss << ';';
        oss << mf.component_a << ',' << mf.component_b << ",1," << mf.ratio_a << ',' << mf.ratio_b;
        const std::string pattern = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
        if (!pattern.empty())
            oss << ',' << pattern;
    }
    return oss.str();
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

bool spectrum_mix_looks_like_snapmaker_custom_entries(const std::string &serialized)
{
    if (serialized.empty())
        return false;
    bool        saw_u = false;
    bool        saw_m = false;
    bool        saw_o = false;
    std::string cur;
    auto        consider = [&](std::string tok) {
        tok = trim_copy(tok);
        if (is_prefixed_int_token(tok, 'u'))
            saw_u = true;
        else if (is_prefixed_int_token(tok, 'm'))
            saw_m = true;
        else if (is_prefixed_int_token(tok, 'o'))
            saw_o = true;
    };
    for (char c : serialized) {
        if (c == ',' || c == ';') {
            consider(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    consider(std::move(cur));
    return saw_u || (saw_m && saw_o);
}

void MixedFilamentManager::load_definitions(const std::string &serialized)
{
    m_mixed.clear();
    if (serialized.empty())
        return;

    // Snapmaker FullSpec 3mf: translate custom-entry grammar, then parse ZR.
    std::string        translated;
    const std::string *src = &serialized;
    if (spectrum_mix_looks_like_snapmaker_custom_entries(serialized)) {
        translated = translate_snapmaker_custom_entries_to_zr(serialized);
        src        = &translated;
        if (src->empty())
            return;
    }

    for (const std::string &row_raw : split_char(*src, ';')) {
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
        bool saw_rc    = false;
        for (size_t i = 5; i < fields.size(); ++i) {
            const std::string token = trim_copy(fields[i]);
            if (token.empty())
                continue;
            if (parse_offset_token(token, mf))
                continue;
            if (parse_ratio_c_token(token, mf)) {
                saw_rc = true;
                continue;
            }
            if (parse_component_c_token(token, mf))
                continue;
            if (parse_gradient_token(token, mf))
                continue;
            if (parse_perimeter_token(token, mf))
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
        if (mf.ratio_c < 0)
            mf.ratio_c = 0;
        // cN without rcN defaults ratio_c to 1. Absent / c0 stays pair-only.
        if (mf.component_c != 0 && !saw_rc)
            mf.ratio_c = 1;

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
        if (mf.component_c != 0)
            oss << ",c" << mf.component_c << ",rc" << mf.ratio_c;
        if (mf.gradient_enabled)
            oss << ",g";
        if (mf.perimeter_modulation)
            oss << ",p";
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
    if (physical == clamp_component(mf->component_b, num_physical))
        return mf->component_b_surface_offset;
    return 0.f;
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
    int ratio_c = (mf->component_c != 0) ? std::max(0, mf->ratio_c) : 0;
    if (ratio_a == 0 && ratio_b == 0 && ratio_c == 0) {
        return clamp_component(mf->component_a, num_physical);
    }

    const int cycle = std::max(1, ratio_a + ratio_b + ratio_c);
    const int pos   = safe_mod(layer_index, cycle);

    unsigned int chosen = mf->component_a;
    if (pos < ratio_a)
        chosen = mf->component_a;
    else if (pos < ratio_a + ratio_b)
        chosen = mf->component_b;
    else if (mf->component_c != 0)
        chosen = mf->component_c;
    else
        chosen = mf->component_b;
    return clamp_component(chosen, num_physical);
}

void MixedFilamentManager::append_physical_0based(unsigned int filament_id_1based, size_t num_physical, std::vector<unsigned int> &out) const
{
    if (filament_id_1based == 0 || num_physical == 0)
        return;

    if (const MixedFilament *mf = mixed_filament_from_id(filament_id_1based, num_physical)) {
        auto append_unique = [&](unsigned int id_1based) {
            if (id_1based == 0)
                return;
            const unsigned int z = id_1based - 1;
            if (std::find(out.begin(), out.end(), z) == out.end())
                out.emplace_back(z);
        };
        append_unique(clamp_component(mf->component_a, num_physical));
        append_unique(clamp_component(mf->component_b, num_physical));
        if (mf->component_c != 0)
            append_unique(clamp_component(mf->component_c, num_physical));
        const std::string pattern = normalize_manual_pattern(mf->manual_pattern);
        for (char token : pattern)
            append_unique(physical_from_pattern_token(token, *mf, num_physical));
        return;
    }

    if (filament_id_1based >= 1 && filament_id_1based <= num_physical)
        out.emplace_back(filament_id_1based - 1);
    else if (filament_id_1based > num_physical)
        out.emplace_back(0); // unknown virtual → first physical (safe fallback)
}

size_t spectrum_paint_id_limit(size_t physical_n, size_t max_filament_id, size_t source_palette_size)
{
    const size_t raw = std::max(physical_n, std::max(max_filament_id, source_palette_size));
    return std::min(SPECTRUM_PAINT_ID_PERSIST_CAP, raw);
}

bool spectrum_volume_extruder_keep(int id, size_t physical_n, size_t max_filament_id)
{
    if (id <= 0)
        return true;
    const size_t uid = size_t(id);
    if (uid <= physical_n)
        return true;
    return uid <= max_filament_id;
}

size_t spectrum_delete_filament_mix_max(const std::string &serialized, size_t post_delete_physical)
{
    return MixedFilamentManager::max_filament_id(serialized, post_delete_physical + 1);
}

void spectrum_stamp_legacy_process_gradient(std::vector<MixedFilament> &rows, bool process_gradient)
{
    if (!process_gradient)
        return;
    for (const MixedFilament &mf : rows) {
        if (mf.gradient_enabled)
            return;
    }
    for (MixedFilament &mf : rows) {
        if (mf.component_c == 0 &&
            MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern).empty())
            mf.gradient_enabled = true;
    }
}

bool mixed_filament_painted_ids_would_shift(const std::string     &old_serialized,
                                            const std::string     &new_serialized,
                                            size_t                 num_physical,
                                            const std::vector<int> &painted_filament_ids,
                                            bool                   process_gradient)
{
    if (num_physical == 0 || painted_filament_ids.empty())
        return false;

    MixedFilamentManager old_mgr;
    MixedFilamentManager new_mgr;
    old_mgr.load_definitions(old_serialized);
    new_mgr.load_definitions(new_serialized);

    bool old_any_g = false;
    for (const MixedFilament &mf : old_mgr.mixed_filaments()) {
        if (mf.gradient_enabled) {
            old_any_g = true;
            break;
        }
    }
    bool new_any_g = false;
    for (const MixedFilament &mf : new_mgr.mixed_filaments()) {
        if (mf.gradient_enabled) {
            new_any_g = true;
            break;
        }
    }
    const bool stamp_world = process_gradient && !old_any_g;

    auto is_pair_capable = [](const MixedFilament &mf) -> bool {
        return mf.component_c == 0 &&
               MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern).empty();
    };

    auto same_mix_core = [](const MixedFilament *lhs, const MixedFilament *rhs) -> bool {
        return lhs->component_a == rhs->component_a && lhs->component_b == rhs->component_b &&
               lhs->component_c == rhs->component_c && lhs->ratio_a == rhs->ratio_a &&
               lhs->ratio_b == rhs->ratio_b && lhs->ratio_c == rhs->ratio_c &&
               lhs->perimeter_modulation == rhs->perimeter_modulation &&
               MixedFilamentManager::normalize_manual_pattern(lhs->manual_pattern) ==
                   MixedFilamentManager::normalize_manual_pattern(rhs->manual_pattern);
    };

    auto same_mix = [&](const MixedFilament *lhs, const MixedFilament *rhs) -> bool {
        if (lhs == nullptr || rhs == nullptr)
            return lhs == rhs;
        if (!same_mix_core(lhs, rhs))
            return false;
        if (lhs->gradient_enabled == rhs->gradient_enabled)
            return true;
        // 0005 stamp / manual g check while process-on and old has no g.
        if (stamp_world && !lhs->gradient_enabled && rhs->gradient_enabled &&
            is_pair_capable(*lhs) && is_pair_capable(*rhs))
            return true;
        return false;
    };

    for (int id : painted_filament_ids) {
        if (id <= int(num_physical))
            continue;
        const MixedFilament *old_mix = old_mgr.mixed_filament_from_id(unsigned(id), num_physical);
        if (old_mix == nullptr)
            continue;
        const MixedFilament *new_mix = new_mgr.mixed_filament_from_id(unsigned(id), num_physical);
        // Pair left untagged while siblings stamped: was interpolating (0005), now ratio.
        if (stamp_world && new_any_g && new_mix != nullptr && is_pair_capable(*old_mix) &&
            is_pair_capable(*new_mix) && !old_mix->gradient_enabled && !new_mix->gradient_enabled &&
            same_mix_core(old_mix, new_mix))
            return true;
        if (!same_mix(old_mix, new_mix))
            return true;
    }
    return false;
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

float spectrum_perimeter_mod_magnitude_mm(float nozzle_mm)
{
    float n = nozzle_mm;
    if (!std::isfinite(n) || n <= 0.f)
        n = 0.4f;
    return std::clamp(0.4f * n, 0.08f, 0.35f);
}

float spectrum_nozzle_mm_for_physical(const std::vector<double> &nozzles, unsigned int physical_1based)
{
    if (nozzles.empty())
        return 0.4f;
    const size_t i = size_t(physical_1based) - 1;
    return (i < nozzles.size()) ? float(nozzles[i]) : float(nozzles.front());
}

float spectrum_perimeter_mod_offset(const MixedFilament &mf, unsigned int physical_1based, float nozzle_mm)
{
    const float xa = mf.component_a_surface_offset;
    const float xb = mf.component_b_surface_offset;
    if (std::abs(xa) > 1e-6f || std::abs(xb) > 1e-6f) {
        if (physical_1based == mf.component_a)
            return xa;
        if (physical_1based == mf.component_b)
            return xb;
        return 0.f;
    }
    if (!mf.perimeter_modulation)
        return 0.f;
    const float mag = spectrum_perimeter_mod_magnitude_mm(nozzle_mm);
    if (physical_1based == mf.component_a)
        return -mag;
    if (physical_1based == mf.component_b)
        return mag;
    return 0.f;
}

namespace {

void consider_opaque_slot(unsigned int id, const std::vector<bool> &opaque_flags, std::vector<unsigned int> &slots)
{
    if (id == 0 || id > opaque_flags.size())
        return;
    if (std::find(slots.begin(), slots.end(), id) == slots.end())
        slots.push_back(id);
}

unsigned int pattern_token_physical_unguarded(char token, const MixedFilament &mf)
{
    if (token == '1')
        return mf.component_a;
    if (token == '2')
        return mf.component_b;
    if (token == '3' && mf.component_c != 0)
        return mf.component_c;
    if (token >= '3' && token <= '9')
        return unsigned(token - '0');
    return 0;
}

std::vector<unsigned int> opaque_blend_slots(const std::vector<bool> &opaque_flags, const MixedFilament &mix)
{
    std::vector<unsigned int> slots;
    const std::string pattern = MixedFilamentManager::normalize_manual_pattern(mix.manual_pattern);
    if (!pattern.empty()) {
        for (char token : pattern)
            consider_opaque_slot(pattern_token_physical_unguarded(token, mix), opaque_flags, slots);
        return slots;
    }
    if (mix.ratio_a > 0)
        consider_opaque_slot(mix.component_a, opaque_flags, slots);
    if (mix.ratio_b > 0)
        consider_opaque_slot(mix.component_b, opaque_flags, slots);
    if (mix.component_c != 0 && mix.ratio_c > 0)
        consider_opaque_slot(mix.component_c, opaque_flags, slots);
    return slots;
}

bool parse_pattern_only_recipe(const std::string &recipe_row, MixedFilament &mix)
{
    const std::string pattern = MixedFilamentManager::normalize_manual_pattern(recipe_row);
    if (pattern.empty())
        return false;
    mix               = MixedFilament{};
    mix.component_a   = 1;
    mix.component_b   = 2;
    mix.manual_pattern = pattern;
    return true;
}

} // namespace

bool spectrum_has_opaque_blend(const std::vector<bool> &opaque_flags, const MixedFilament &mix)
{
    const std::vector<unsigned int> slots = opaque_blend_slots(opaque_flags, mix);
    if (slots.size() < 2)
        return false;
    for (unsigned int id : slots) {
        if (opaque_flags[size_t(id) - 1])
            return true;
    }
    return false;
}

bool spectrum_has_opaque_blend(const std::vector<bool> &opaque_flags, const std::string &recipe_row)
{
    MixedFilamentManager mgr;
    mgr.load_definitions(recipe_row);
    if (!mgr.mixed_filaments().empty()) {
        for (const MixedFilament &mf : mgr.mixed_filaments()) {
            if (spectrum_has_opaque_blend(opaque_flags, mf))
                return true;
        }
        return false;
    }
    MixedFilament mix;
    if (!parse_pattern_only_recipe(recipe_row, mix))
        return false;
    return spectrum_has_opaque_blend(opaque_flags, mix);
}

std::string spectrum_opaque_blend_marker(const std::vector<bool> &opaque_flags,
                                         const MixedFilament     &mix,
                                         bool                     lut_loaded)
{
    if (!spectrum_has_opaque_blend(opaque_flags, mix))
        return {};
    if (lut_loaded)
        return std::string("measured ") + SPECTRUM_OPAQUE_BLEND_MARKER;
    return SPECTRUM_OPAQUE_BLEND_MARKER;
}

std::string spectrum_opaque_blend_marker(const std::vector<bool> &opaque_flags,
                                         const std::string       &recipe_row,
                                         bool                     lut_loaded)
{
    MixedFilamentManager mgr;
    mgr.load_definitions(recipe_row);
    if (!mgr.mixed_filaments().empty()) {
        for (const MixedFilament &mf : mgr.mixed_filaments()) {
            std::string marker = spectrum_opaque_blend_marker(opaque_flags, mf, lut_loaded);
            if (!marker.empty())
                return marker;
        }
        return {};
    }
    MixedFilament mix;
    if (!parse_pattern_only_recipe(recipe_row, mix))
        return {};
    return spectrum_opaque_blend_marker(opaque_flags, mix, lut_loaded);
}

std::string spectrum_with_opaque_blend_marker(const std::string       &label,
                                              const std::vector<bool> &opaque_flags,
                                              const MixedFilament     &mix,
                                              bool                     lut_loaded)
{
    const std::string marker = spectrum_opaque_blend_marker(opaque_flags, mix, lut_loaded);
    if (marker.empty())
        return label;
    if (label.empty())
        return marker;
    return label + "  " + marker;
}

} // namespace Slic3r
