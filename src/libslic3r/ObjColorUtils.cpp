#include "ObjColorUtils.hpp"

bool obj_color_deal_algo(std::vector<Slic3r::RGBA> & input_colors,
                         std::vector<Slic3r::RGBA> & cluster_colors_from_algo,
                         std::vector<int> &         cluster_labels_from_algo,
                         char &                     cluster_number,
                         int                        max_cluster)
{
    QuantKMeans quant(10);
    quant.apply(input_colors, cluster_colors_from_algo, cluster_labels_from_algo, (int) cluster_number, max_cluster);
    if (cluster_number == -1) {
        return false;
    }
    return true;
}