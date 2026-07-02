#pragma once

#include "PluginSource.hpp"
#include "PluginStatus.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r::GUI
{
    enum class PluginSortKey
    {
        Status,
        Name,
        Source
    };

    enum class PluginSortOrder
    {
        Asc,
        Desc
    };

    inline std::string to_string(PluginSortKey sort_key)
    {
        switch (sort_key)
        {
        case PluginSortKey::Status: return "status";
        case PluginSortKey::Name: return "name";
        case PluginSortKey::Source: return "source";
        }

        return "status";
    }

    inline std::string to_string(PluginSortOrder sort_order)
    {
        return sort_order == PluginSortOrder::Desc ? "desc" : "asc";
    }

    inline PluginSortKey plugin_sort_key_from_string(const std::string& sort_key, PluginSortKey fallback)
    {
        if (sort_key == "status")
            return PluginSortKey::Status;
        if (sort_key == "name")
            return PluginSortKey::Name;
        if (sort_key == "source")
            return PluginSortKey::Source;
        return fallback;
    }

    inline PluginSortOrder plugin_sort_order_from_string(const std::string& sort_order, PluginSortOrder fallback)
    {
        if (sort_order == "asc")
            return PluginSortOrder::Asc;
        if (sort_order == "desc")
            return PluginSortOrder::Desc;
        return fallback;
    }

    // Natural, case-insensitive ASCII compare returning -1 / 0 / +1. Digit runs compare by
    // numeric value; other chars compare lowercased; on a prefix tie the shorter string is less.
    //   e.g. "item2"  < "item10"   (2 < 10, not '2' > '1')
    //        "Camera" == "camera"  (case ignored)
    //        "app"    < "apple"    (prefix is shorter)
    //        "1"      < "01"       (equal value, fewer leading zeros wins the tie)
    // note: ASCII only - no locale/Unicode; accented or non-Latin names fall back to byte order.
    inline int compare_ascii_case_insensitive_natural(const std::string& lhs, const std::string& rhs)
    {
        std::size_t li = 0;
        std::size_t ri = 0;

        while (li < lhs.size() && ri < rhs.size())
        {
            const unsigned char lc = static_cast<unsigned char>(lhs[li]);
            const unsigned char rc = static_cast<unsigned char>(rhs[ri]);

            if (std::isdigit(lc) && std::isdigit(rc))
            {
                const std::size_t lhs_digit_begin = li;
                const std::size_t rhs_digit_begin = ri;
                while (li < lhs.size() && std::isdigit(static_cast<unsigned char>(lhs[li])))
                    ++li;
                while (ri < rhs.size() && std::isdigit(static_cast<unsigned char>(rhs[ri])))
                    ++ri;

                const std::string_view lhs_run(lhs.data() + lhs_digit_begin, li - lhs_digit_begin);
                const std::string_view rhs_run(rhs.data() + rhs_digit_begin, ri - rhs_digit_begin);
                // why: digit runs compare numerically; leading zeros only break exact ties ("1" < "01").
                const std::string_view lhs_num = lhs_run.substr(std::min(lhs_run.find_first_not_of('0'), lhs_run.size()));
                const std::string_view rhs_num = rhs_run.substr(std::min(rhs_run.find_first_not_of('0'), rhs_run.size()));
                if (lhs_num.size() != rhs_num.size())
                    return lhs_num.size() < rhs_num.size() ? -1 : 1;
                if (const int cmp = lhs_num.compare(rhs_num); cmp != 0)
                    return cmp;
                // note: fewer-leading-zeros-first is our convention, not an industry standard (impls
                //   diverge here); it only matters as a deterministic total order for unstable std::sort.
                if (lhs_run.size() != rhs_run.size())
                    return lhs_run.size() < rhs_run.size() ? -1 : 1;
                continue;
            }

            const int lower_lhs = std::tolower(lc);
            const int lower_rhs = std::tolower(rc);
            if (lower_lhs != lower_rhs)
                return lower_lhs < lower_rhs ? -1 : 1;

            ++li;
            ++ri;
        }

        if (li == lhs.size() && ri == rhs.size())
            return 0;
        return li == lhs.size() ? -1 : 1;
    }

    // Neutral baseline order used as the tie-breaker under every primary sort key. Always
    // ascending: source priority, then type_key, then display_name, then plugin_key.
    //   e.g. two items with equal Status sort by source priority (Mine, then Subscribed,
    //        then Local), then by name.
    template <class PluginItem>
    int compare_plugin_base_order(const PluginItem& lhs, const PluginItem& rhs)
    {
        // why: source ties use the declared PluginSource ordinal priority - the same order the
        //   Source sort key uses - so the neutral baseline never contradicts it.
        if (const int cmp = static_cast<int>(lhs.source) - static_cast<int>(rhs.source); cmp != 0)
            return cmp;
        if (const int cmp = lhs.type_key.compare(rhs.type_key); cmp != 0)
            return cmp;
        if (const int cmp = lhs.display_name.compare(rhs.display_name); cmp != 0)
            return cmp;
        return lhs.plugin_key.compare(rhs.plugin_key);
    }

    // Compares two items by the chosen primary key, returning -1 / 0 / +1. Status and Source
    // rank by enum ordinal (the declared dialog priority); Name uses the natural compare above.
    //   e.g. Status: an enabled item (lower ordinal) sorts before a disabled one.
    //        Name:   "Plugin 2" sorts before "Plugin 10".
    template <class PluginItem>
    int compare_plugin_sort_key(const PluginItem& lhs, const PluginItem& rhs, PluginSortKey sort_key)
    {
        switch (sort_key)
        {
        case PluginSortKey::Status:
            // why: PluginStatus/PluginSource declare the dialog sort priority as their ordinal order.
            return static_cast<int>(lhs.status) - static_cast<int>(rhs.status);
        case PluginSortKey::Name:
            return compare_ascii_case_insensitive_natural(lhs.display_name, rhs.display_name);
        case PluginSortKey::Source:
            return static_cast<int>(lhs.source) - static_cast<int>(rhs.source);
        }

        return 0;
    }

    // Sorts the dialog list in place by primary key + direction. Ties always fall back to the
    // ascending base order, so the result is deterministic regardless of the primary direction.
    //   e.g. sort_key=Name, order=Desc -> names Z..A, but equal names keep the stable base order.
    template <class PluginItem>
    void sort_plugin_items_for_dialog(std::vector<PluginItem>& items, PluginSortKey sort_key,
                                      PluginSortOrder sort_order)
    {
        std::sort(items.begin(), items.end(),
                  [sort_key, sort_order](const PluginItem& lhs, const PluginItem& rhs)
                  {
                      if (const int cmp = compare_plugin_sort_key(lhs, rhs, sort_key); cmp != 0)
                          return sort_order == PluginSortOrder::Asc ? cmp < 0 : cmp > 0;
                      // why: ties fall back to ascending base order regardless of the primary direction.
                      return compare_plugin_base_order(lhs, rhs) < 0;
                  });
    }
} // namespace Slic3r::GUI
