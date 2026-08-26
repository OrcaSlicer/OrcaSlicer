#include "MixedFilamentMatch.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace Slic3r {

namespace {

// Tiny D65 sRGB→Lab / CIE76. Copied locally because ColorSpaceConvert.hpp names wxColour.
double pivot_rgb(double n)
{
    return (n > 0.04045 ? std::pow((n + 0.055) / 1.055, 2.4) : n / 12.92) * 100.0;
}

double pivot_xyz(double n)
{
    const double i = std::cbrt(n);
    return n > 0.008856 ? i : 7.787 * n + 16.0 / 116.0;
}

void rgb_to_xyz(float R, float G, float B, float *X, float *Y, float *Z)
{
    R = float(pivot_rgb(R));
    G = float(pivot_rgb(G));
    B = float(pivot_rgb(B));
    *X = 0.412453f * R + 0.357580f * G + 0.180423f * B;
    *Y = 0.212671f * R + 0.715160f * G + 0.072169f * B;
    *Z = 0.019334f * R + 0.119193f * G + 0.950227f * B;
}

void xyz_to_lab(float X, float Y, float Z, float *L, float *a, float *b)
{
    const double x = pivot_xyz(X / 95.047);
    const double y = pivot_xyz(Y / 100.000);
    const double z = pivot_xyz(Z / 108.883);
    *L = float(116.0 * y - 16.0);
    *a = float(500.0 * (x - y));
    *b = float(200.0 * (y - z));
}

void rgb_to_lab(const ColorRGB &c, float *L, float *a, float *b)
{
    float X = 0.f, Y = 0.f, Z = 0.f;
    rgb_to_xyz(c.r(), c.g(), c.b(), &X, &Y, &Z);
    xyz_to_lab(X, Y, Z, L, a, b);
}

float cie76(const ColorRGB &u, const ColorRGB &v)
{
    float L1 = 0.f, a1 = 0.f, b1 = 0.f;
    float L2 = 0.f, a2 = 0.f, b2 = 0.f;
    rgb_to_lab(u, &L1, &a1, &b1);
    rgb_to_lab(v, &L2, &a2, &b2);
    const float dL = L1 - L2;
    const float da = a1 - a2;
    const float db = b1 - b2;
    return std::sqrt(dL * dL + da * da + db * db);
}

float srgb_to_linear(float c)
{
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float c)
{
    c = std::clamp(c, 0.f, 1.f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

ColorRGB yule_nielsen_n3(const std::vector<ColorRGB> &cols, const std::vector<float> &weights)
{
    if (cols.empty() || cols.size() != weights.size())
        return ColorRGB::BLACK();

    float wsum = 0.f;
    for (float w : weights)
        wsum += std::max(0.f, w);
    if (wsum <= 0.f)
        return ColorRGB::BLACK();

    const float inv_n = 1.f / 3.f;
    float lin[3] = {0.f, 0.f, 0.f};
    for (size_t i = 0; i < cols.size(); ++i) {
        const float w = std::max(0.f, weights[i]) / wsum;
        if (w <= 0.f)
            continue;
        const float ch[3] = {cols[i].r(), cols[i].g(), cols[i].b()};
        for (int k = 0; k < 3; ++k)
            lin[k] += w * std::pow(srgb_to_linear(ch[k]), inv_n);
    }
    return ColorRGB(linear_to_srgb(std::pow(lin[0], 3.f)),
                    linear_to_srgb(std::pow(lin[1], 3.f)),
                    linear_to_srgb(std::pow(lin[2], 3.f)));
}

unsigned clamp_physical_id(unsigned id, size_t n)
{
    if (n == 0)
        return 1;
    if (id < 1)
        return 1;
    if (id > n)
        return unsigned(n);
    return id;
}

std::vector<float> resolve_td_scale(size_t n, const std::vector<float> *td)
{
    std::vector<float> scale(n, 1.f);
    if (td != nullptr && td->size() == n) {
        for (size_t i = 0; i < n; ++i)
            scale[i] = ((*td)[i] > 0.f) ? (1.f / (*td)[i]) : 1.f;
        return scale;
    }
    // Default Panchroma TDs only when the live set is 4 slots.
    if (td == nullptr && n == 4) {
        const float panchroma_td[4] = {8.f, 7.8f, 14.f, 11.5f};
        for (size_t i = 0; i < 4; ++i)
            scale[i] = 1.f / panchroma_td[i];
    }
    return scale;
}

int gcd_positive(int a, int b)
{
    return int(std::gcd(unsigned(std::abs(a)), unsigned(std::abs(b))));
}

void gcd_reduce_ratios(int &ra, int &rb, int &rc)
{
    int g = gcd_positive(ra, rb);
    g = gcd_positive(g, rc);
    if (g > 1) {
        ra /= g;
        rb /= g;
        if (rc != 0)
            rc /= g;
    }
}

std::string serialize_mix_recipe(const MixedFilament &mf)
{
    std::ostringstream oss;
    oss << mf.component_a << ',' << mf.component_b << ',' << (mf.enabled ? 1 : 0)
        << ',' << mf.ratio_a << ',' << mf.ratio_b;
    if (mf.component_c != 0)
        oss << ",c" << mf.component_c << ",rc" << mf.ratio_c;
    MixedFilamentManager mgr;
    mgr.load_definitions(oss.str());
    return mgr.serialize_definitions();
}

int result_period(const MixMatchResult &r)
{
    if (!r.valid)
        return 1000;
    if (r.kind == MixMatchResult::Kind::Physical)
        return 1;
    int p = r.mix.ratio_a + r.mix.ratio_b;
    if (r.mix.component_c != 0)
        p += r.mix.ratio_c;
    return std::max(1, p);
}

} // namespace

std::string normalize_mix_match_hex(const std::string &text)
{
    size_t b = 0;
    while (b < text.size() && std::isspace(static_cast<unsigned char>(text[b])))
        ++b;
    size_t e = text.size();
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1])))
        --e;
    std::string s = text.substr(b, e - b);
    if (!s.empty() && s.front() == '#')
        s.erase(s.begin());
    if (s.size() != 6 && s.size() != 8)
        return {};
    for (char &c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return {};
        c = char(std::toupper(static_cast<unsigned char>(c)));
    }
    return std::string("#") + s;
}

ColorRGB predicted_swatch_for_mix(const MixedFilament         &mf,
                                  const std::vector<ColorRGB> &physicals,
                                  const std::vector<float>    *td)
{
    const size_t n = physicals.size();
    if (n == 0)
        return ColorRGB::BLACK();

    const std::vector<float> td_scale = resolve_td_scale(n, td);
    auto weight_for = [&](unsigned id_1based, int layers) -> float {
        if (layers <= 0)
            return 0.f;
        const unsigned id = clamp_physical_id(id_1based, n);
        return float(layers) * td_scale[size_t(id - 1)];
    };

    std::vector<ColorRGB> cols;
    std::vector<float>    weights;
    const unsigned a = clamp_physical_id(mf.component_a, n);
    const unsigned b = clamp_physical_id(mf.component_b, n);
    cols.push_back(physicals[size_t(a - 1)]);
    weights.push_back(weight_for(a, mf.ratio_a));
    cols.push_back(physicals[size_t(b - 1)]);
    weights.push_back(weight_for(b, mf.ratio_b));
    if (mf.component_c != 0) {
        const unsigned c = clamp_physical_id(mf.component_c, n);
        const int      rc = (mf.ratio_c > 0) ? mf.ratio_c : 1;
        cols.push_back(physicals[size_t(c - 1)]);
        weights.push_back(weight_for(c, rc));
    }
    return yule_nielsen_n3(cols, weights);
}

MixMatchResult match_printable_mix(const ColorRGB              &target,
                                   const std::vector<ColorRGB> &physicals,
                                   const std::vector<float>    *td,
                                   int                          period_cap)
{
    MixMatchResult best;
    // This track's printable lattice is C/M/Y/K slots 1–4. Extra physicals are ignored.
    const size_t n = std::min(physicals.size(), size_t(4));
    if (n == 0)
        return best;
    const std::vector<ColorRGB> slots(physicals.begin(), physicals.begin() + int(n));
    std::vector<float>          td_prefix;
    const std::vector<float>   *td_use = td;
    if (td != nullptr && td->size() != n) {
        if (td->size() >= n) {
            td_prefix.assign(td->begin(), td->begin() + int(n));
            td_use = &td_prefix;
        } else {
            td_use = nullptr;
        }
    }

    const int cap = std::max(0, period_cap);

    auto consider = [&](MixMatchResult cand, int period) {
        if (!best.valid || cand.distance < best.distance ||
            (cand.distance == best.distance && period < result_period(best))) {
            best = std::move(cand);
        }
    };

    auto consider_physical = [&](unsigned id_1based) {
        MixMatchResult cand;
        cand.valid       = true;
        cand.kind        = MixMatchResult::Kind::Physical;
        cand.physical_id = id_1based;
        cand.predicted   = slots[size_t(id_1based - 1)];
        cand.distance    = cie76(cand.predicted, target);
        consider(std::move(cand), 1);
    };

    auto consider_mix = [&](const MixedFilament &mf, int period) {
        MixMatchResult cand;
        cand.valid      = true;
        cand.kind       = MixMatchResult::Kind::Mix;
        cand.mix        = mf;
        cand.recipe_row = serialize_mix_recipe(mf);
        cand.predicted  = predicted_swatch_for_mix(mf, slots, td_use);
        cand.distance   = cie76(cand.predicted, target);
        consider(std::move(cand), period);
    };

    for (size_t i = 0; i < n; ++i)
        consider_physical(unsigned(i + 1));

    using Key = std::tuple<unsigned, unsigned, unsigned, int, int, int>;
    std::set<Key> seen;

    if (n >= 2 && cap >= 2) {
        static const int k_pair[][2] = {{1, 1}, {2, 1}, {1, 2}, {3, 1}, {1, 3}, {2, 2}};
        for (size_t i = 0; i + 1 < n; ++i) {
            for (size_t j = i + 1; j < n; ++j) {
                for (const auto &ratio : k_pair) {
                    int ra = ratio[0];
                    int rb = ratio[1];
                    int rc = 0;
                    gcd_reduce_ratios(ra, rb, rc); // 2:2 → 1:1 so we never serialize 1,2,1,2,2
                    const int period = ra + rb;
                    if (period < 1 || period > cap)
                        continue;
                    const Key key{unsigned(i + 1), unsigned(j + 1), 0u, ra, rb, 0};
                    if (!seen.insert(key).second)
                        continue;
                    MixedFilament mf;
                    mf.component_a = unsigned(i + 1);
                    mf.component_b = unsigned(j + 1);
                    mf.component_c = 0;
                    mf.ratio_a     = ra;
                    mf.ratio_b     = rb;
                    mf.ratio_c     = 0;
                    mf.enabled     = true;
                    consider_mix(mf, period);
                }
            }
        }
    }

    if (n >= 3 && cap >= 3) {
        static const int k_triple[][3] = {{1, 1, 1}, {2, 1, 1}, {1, 2, 1}, {1, 1, 2}};
        for (size_t i = 0; i + 2 < n; ++i) {
            for (size_t j = i + 1; j + 1 < n; ++j) {
                for (size_t k = j + 1; k < n; ++k) {
                    for (const auto &ratio : k_triple) {
                        int ra = ratio[0];
                        int rb = ratio[1];
                        int rc = ratio[2];
                        gcd_reduce_ratios(ra, rb, rc);
                        const int period = ra + rb + rc;
                        if (period < 1 || period > cap)
                            continue;
                        const Key key{unsigned(i + 1), unsigned(j + 1), unsigned(k + 1), ra, rb, rc};
                        if (!seen.insert(key).second)
                            continue;
                        MixedFilament mf;
                        mf.component_a = unsigned(i + 1);
                        mf.component_b = unsigned(j + 1);
                        mf.component_c = unsigned(k + 1);
                        mf.ratio_a     = ra;
                        mf.ratio_b     = rb;
                        mf.ratio_c     = rc;
                        mf.enabled     = true;
                        consider_mix(mf, period);
                    }
                }
            }
        }
    }

    // Neutral targets darker than every physical: a short CMY YN mud can beat Grey
    // on CIE76 for #000000. Spec still requires the nearest physical (Grey), not a stack.
    if (best.valid && n > 0) {
        float tL = 0.f, ta = 0.f, tb = 0.f;
        rgb_to_lab(target, &tL, &ta, &tb);
        const float tchroma = std::sqrt(ta * ta + tb * tb);
        float min_L = 1e30f;
        for (size_t i = 0; i < n; ++i) {
            float L = 0.f, a = 0.f, b = 0.f;
            rgb_to_lab(slots[i], &L, &a, &b);
            min_L = std::min(min_L, L);
        }
        if (tchroma < 8.f && tL + 1.f < min_L) {
            MixMatchResult phys_best;
            for (size_t i = 0; i < n; ++i) {
                MixMatchResult cand;
                cand.valid       = true;
                cand.kind        = MixMatchResult::Kind::Physical;
                cand.physical_id = unsigned(i + 1);
                cand.predicted   = slots[i];
                cand.distance    = cie76(cand.predicted, target);
                if (!phys_best.valid || cand.distance < phys_best.distance)
                    phys_best = std::move(cand);
            }
            if (phys_best.valid)
                best = std::move(phys_best);
        }
    }

    return best;
}

} // namespace Slic3r
