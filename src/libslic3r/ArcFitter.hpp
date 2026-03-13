#ifndef slic3r_ArcFitter_hpp_
#define slic3r_ArcFitter_hpp_

#include "Circle.hpp"

namespace Slic3r {

//BBS: linear move(G0 and G1) or arc move(G2 and G3).
enum class EMovePathType : unsigned char
{
    Noop_move,
    Linear_move,
    Arc_move_cw,
    Arc_move_ccw,
    Count
};

//BBS
struct PathFittingData{
    size_t start_point_index;
    size_t end_point_index;
    EMovePathType path_type;
    // BBS: only valid when path_type is arc move
    // Used to store detail information of arc segment
    ArcSegment arc_data;

    bool is_linear_move() {
        return (path_type == EMovePathType::Linear_move);
    }
    bool is_arc_move() {
        return (path_type == EMovePathType::Arc_move_ccw || path_type == EMovePathType::Arc_move_cw);
    }
    bool reverse_arc_path() {
        if (!is_arc_move() || !arc_data.reverse())
            return false;
        path_type = (arc_data.direction == ArcDirection::Arc_Dir_CCW) ? EMovePathType::Arc_move_ccw : EMovePathType::Arc_move_cw;
        return true;
    }
};

class ArcFitter {
public:
    //BBS: this function is used to check the point list and return which part can fit as arc, which part should be line
    static void do_arc_fitting(const Points& points, std::vector<PathFittingData> &result, double tolerance);
    //BBS: this function is used to check the point list and return which part can fit as arc, which part should be line.
    //By the way, it also use DP simplify to reduce point of straight part and only keep the start and end point of arc.
    static void do_arc_fitting_and_simplify(Points& points, std::vector<PathFittingData>& result, double tolerance);
};

}


#endif
