#include "Outcome.hpp"

#include "slic3r/GUI/I18N.hpp"

#include <boost/format.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <wx/string.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

// `text` with its %1%.. placeholders filled. A message and its values that do not match (a
// translation that drops a placeholder) still give text rather than an exception.
std::string filled(const std::string& text, const std::vector<std::string>& args)
{
    if (args.empty())
        return text;
    boost::format format(text);
    format.exceptions(boost::io::no_error_bits);
    for (const std::string& arg : args)
        format % arg;
    return format.str();
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// The fewest insertions, deletions and substitutions of one character that turn `a` into `b`.
std::size_t edit_distance(const std::string& a, const std::string& b)
{
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
        row[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0]               = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            row[j]                  = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diagonal                = above;
        }
    }
    return row[b.size()];
}

} // namespace

wxString Refusal::translated() const { return wxString::FromUTF8(filled(I18N::translate_utf8(msgid), args)); }

std::string Refusal::english() const { return filled(msgid, args); }

std::vector<std::string> closest_names(std::vector<std::string> candidates, const std::string& name)
{
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    const std::string        wanted = lower(name);
    std::vector<std::string> words;
    for (std::size_t start = 0; start < wanted.size();) {
        const std::size_t end = std::min(wanted.find(' ', start), wanted.size());
        if (end > start)
            words.push_back(wanted.substr(start, end - start));
        start = end + 1;
    }
    std::vector<std::tuple<int, std::size_t, std::string>> ranked;
    for (std::string& candidate : candidates) {
        const std::string low   = lower(candidate);
        const auto        holds = [&low](const std::string& word) { return low.find(word) != std::string::npos; };
        const int         found = int(std::count_if(words.begin(), words.end(), holds));
        ranked.emplace_back(-found, edit_distance(wanted, low), std::move(candidate));
    }
    std::sort(ranked.begin(), ranked.end());
    std::vector<std::string> names;
    for (auto& [found, distance, candidate] : ranked)
        if (names.size() < 8)
            names.push_back(std::move(candidate));
    return names;
}

}}} // namespace Slic3r::GUI::Ops
