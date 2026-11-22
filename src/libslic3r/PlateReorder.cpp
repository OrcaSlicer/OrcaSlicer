#include "PlateReorder.hpp"

#include <algorithm>
#include <cmath>

namespace Slic3r::PlateReorder {

Target drop_target(int count, int cols, double col, double row, double plate_width)
{
    const int rows   = (count + cols - 1) / cols;
    const int r      = std::clamp(int(std::floor(row)), 0, rows - 1);
    const int c      = std::clamp(int(std::floor(col)), 0, cols - 1);
    Target    target;
    target.plate     = r * cols + c;
    target.after     = col - c > plate_width * 0.5;
    if (target.plate >= count) {
        target.plate = count - 1;
        target.after = true;
    }
    target.insert = target.plate + (target.after ? 1 : 0);
    return target;
}

int index_after_insert(int from, int insert)
{
    if (insert == from || insert == from + 1)
        return -1;
    return insert > from ? insert - 1 : insert;
}

} // namespace Slic3r::PlateReorder
