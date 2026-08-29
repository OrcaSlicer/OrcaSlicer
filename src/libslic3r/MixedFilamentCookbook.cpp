#include "MixedFilamentCookbook.hpp"

#include <algorithm>
#include <cstdlib>
#include <numeric>

namespace Slic3r {

namespace {

int gcd_positive(int a, int b)
{
    return int(std::gcd(unsigned(std::abs(a)), unsigned(std::abs(b))));
}

void gcd_reduce_ratios(int &ra, int &rb, int &rc)
{
    int g = gcd_positive(ra, rb);
    g     = gcd_positive(g, rc);
    if (g > 1) {
        ra /= g;
        rb /= g;
        if (rc != 0)
            rc /= g;
    }
}

MixedFilament make_pair(unsigned a, unsigned b, int ra, int rb)
{
    MixedFilament mf;
    mf.component_a = a;
    mf.component_b = b;
    mf.component_c = 0;
    mf.ratio_a     = ra;
    mf.ratio_b     = rb;
    mf.ratio_c     = 0;
    mf.enabled     = true;
    return mf;
}

MixedFilament make_triple(unsigned a, unsigned b, unsigned c, int ra, int rb, int rc)
{
    MixedFilament mf;
    mf.component_a = a;
    mf.component_b = b;
    mf.component_c = c;
    mf.ratio_a     = ra;
    mf.ratio_b     = rb;
    mf.ratio_c     = rc;
    mf.enabled     = true;
    return mf;
}

bool recipe_fits(const MixedFilament &mf, size_t n_use)
{
    if (mf.component_a == 0 || mf.component_a > n_use)
        return false;
    if (mf.component_b == 0 || mf.component_b > n_use)
        return false;
    if (mf.component_c != 0 && mf.component_c > n_use)
        return false;
    return true;
}

size_t count_enabled(const std::vector<MixedFilament> &rows)
{
    size_t n = 0;
    for (const MixedFilament &mf : rows) {
        if (mf.enabled)
            ++n;
    }
    return n;
}

} // namespace

std::vector<MixedFilament> spectrum_cookbook_recipes(size_t num_physical)
{
    const size_t n_use = std::min(num_physical, size_t(4));
    if (n_use < 2)
        return {};

    // Phase A — C(n_use,2) 1:1 pairs, nested i < j (0006 order).
    static const unsigned pair_a[] = {1, 1, 2, 1, 2, 3};
    static const unsigned pair_b[] = {2, 3, 3, 4, 4, 4};

    std::vector<MixedFilament> out;
    out.reserve(11);
    for (size_t i = 0; i < 6; ++i) {
        MixedFilament mf = make_pair(pair_a[i], pair_b[i], 1, 1);
        if (recipe_fits(mf, n_use))
            out.push_back(mf);
    }

    // Phase B — period-4 extras; skip if any component > n_use.
    static const MixedFilament extras[] = {
        make_pair(1, 3, 1, 2),
        make_pair(2, 3, 1, 2),
        make_pair(1, 2, 1, 2),
        make_triple(1, 2, 3, 1, 1, 1),
        make_triple(1, 2, 3, 1, 1, 2),
    };
    for (const MixedFilament &mf : extras) {
        if (recipe_fits(mf, n_use))
            out.push_back(mf);
    }
    return out;
}

bool spectrum_cookbook_same_recipe(const MixedFilament &a, const MixedFilament &b)
{
    if (a.component_a != b.component_a || a.component_b != b.component_b || a.component_c != b.component_c)
        return false;

    int ra = a.ratio_a;
    int rb = a.ratio_b;
    int rc = (a.component_c == 0) ? 0 : a.ratio_c;
    int sa = b.ratio_a;
    int sb = b.ratio_b;
    int sc = (b.component_c == 0) ? 0 : b.ratio_c;
    gcd_reduce_ratios(ra, rb, rc);
    gcd_reduce_ratios(sa, sb, sc);
    return ra == sa && rb == sb && rc == sc;
}

MixCookbookAppend spectrum_cookbook_append(
    const std::vector<MixedFilament> &existing,
    size_t                            num_physical,
    size_t                            persist_cap)
{
    MixCookbookAppend result;
    const auto        recipes = spectrum_cookbook_recipes(num_physical);
    size_t            enabled = count_enabled(existing);

    for (const MixedFilament &recipe : recipes) {
        bool dup = false;
        for (const MixedFilament &row : existing) {
            if (spectrum_cookbook_same_recipe(recipe, row)) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            for (const MixedFilament &row : result.added) {
                if (spectrum_cookbook_same_recipe(recipe, row)) {
                    dup = true;
                    break;
                }
            }
        }
        if (dup) {
            ++result.skipped_duplicate;
            continue;
        }
        if (num_physical + enabled + 1 > persist_cap) {
            ++result.skipped_cap;
            break;
        }
        result.added.push_back(recipe);
        if (recipe.enabled)
            ++enabled;
    }
    return result;
}

} // namespace Slic3r
