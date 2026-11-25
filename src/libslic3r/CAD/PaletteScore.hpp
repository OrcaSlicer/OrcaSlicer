#ifndef slic3r_CAD_PaletteScore_hpp_
#define slic3r_CAD_PaletteScore_hpp_

#include <cctype>
#include <string>
#include <cstddef>

namespace Slic3r {

// Fuzzy rank of a command label against what the user typed in the command palette (B-palette).
// -1 = no match. Order of worth: prefix > word start > substring > letters in order; a usable
// command gets +150 so a greyed one never outranks an equally good enabled one. An empty query
// matches everything at 0 (the caller then lists recents first).
inline int palette_score(const std::string& query, const std::string& label, bool enabled)
{
    auto low = [](std::string s) { for (char& c : s) c = char(std::tolower((unsigned char) c)); return s; };
    const std::string q = low(query), l = low(label);
    if (q.empty()) return enabled ? 150 : 0;
    int base = -1;
    const size_t at = l.find(q);
    if (at == 0)                                              base = 1000;
    else if (at != std::string::npos && !std::isalnum((unsigned char) l[at - 1])) base = 800;
    else if (at != std::string::npos)                         base = 600;
    else {   // in order, not adjacent
        size_t i = 0;
        for (char c : l) if (i < q.size() && c == q[i]) ++i;
        if (i == q.size()) base = 300;
    }
    if (base < 0) return -1;
    return base - int(l.size() > 99 ? 99 : l.size()) + (enabled ? 150 : 0);   // shorter label first on a tie
}

} // namespace Slic3r

#endif
