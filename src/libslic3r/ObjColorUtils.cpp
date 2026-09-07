#include "ObjColorUtils.hpp"

#include <algorithm>

bool obj_color_deal_algo(std::vector<Slic3r::RGBA> & input_colors,
                         std::vector<Slic3r::RGBA> & cluster_colors_from_algo,
                         std::vector<int> &         cluster_labels_from_algo,
                         char &                     cluster_number,
                         int                        max_cluster,
                         bool                       preserve_input_colors)
{
    cluster_colors_from_algo.clear();
    cluster_labels_from_algo.clear();
    if (input_colors.empty() || max_cluster < 1)
        return false;

    if (preserve_input_colors) {
        // Generated OBJ colors are already a confirmed palette. Keep exact RGB
        // groups (including close colors) instead of running them through Lab/K-means.
        cluster_labels_from_algo.reserve(input_colors.size());
        for (const auto& color : input_colors) {
            auto found = std::find(cluster_colors_from_algo.begin(), cluster_colors_from_algo.end(), color);
            const int label = int(std::distance(cluster_colors_from_algo.begin(), found));
            if (found == cluster_colors_from_algo.end()) {
                cluster_colors_from_algo.push_back(color);
                if (cluster_colors_from_algo.size() > size_t(max_cluster))
                    break;
            }
            cluster_labels_from_algo.push_back(label);
        }
        if (cluster_colors_from_algo.size() <= size_t(max_cluster) &&
            (cluster_number < 1 || size_t(cluster_number) >= cluster_colors_from_algo.size()))
            return cluster_number != -1;
        // An explicit smaller count, or an input exceeding the dialog's capacity,
        // still uses the existing quantizer.
    }
    QuantKMeans quant(10);
    quant.apply(input_colors, cluster_colors_from_algo, cluster_labels_from_algo, (int) cluster_number, max_cluster);
    if (cluster_number == -1) {
        return false;
    }
    return true;
}
