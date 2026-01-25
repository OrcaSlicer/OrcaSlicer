#ifndef slic3r_ConflictChecker_hpp_
#define slic3r_ConflictChecker_hpp_

#include "../Utils.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../Layer.hpp"

#include <queue>
#include <vector>
#include <optional>

namespace Slic3r {

struct LineWithID
{
    Line          _line;
    const void *  _id;
    ExtrusionRole _role;

    LineWithID(const Line &line, const void* id, ExtrusionRole role) : _line(line), _id(id), _role(role) {}
};

using LineWithIDs = std::vector<LineWithID>;

struct ExtrusionLayer
{
    ExtrusionPaths paths;
    const Layer *  layer;
    float          bottom_z;
    float          height;
};

enum class ExtrusionLayersType { INFILL, PERIMETERS, SUPPORT, WIPE_TOWER };

class ExtrusionLayers : public std::vector<ExtrusionLayer>
{
public:
    ExtrusionLayersType type;
};

struct ObjectExtrusions
{
    ExtrusionLayers perimeters;
    ExtrusionLayers support;

    ObjectExtrusions()
    {
        perimeters.type = ExtrusionLayersType::PERIMETERS;
        support.type    = ExtrusionLayersType::SUPPORT;
    }
};

class LinesBucket
{
public:
    float    _curBottomZ = 0.0;
    unsigned _curPileIdx = 0;

    ExtrusionLayers _piles;
    const void*     _id;
    Point           _offset;

public:
    LinesBucket(ExtrusionLayers &&paths, const void* id, Point offset) : _piles(paths), _id(id), _offset(offset) {}
    LinesBucket(LinesBucket &&) = default;

    std::pair<int, int> curRange() const
    {
        auto begin = std::lower_bound(_piles.begin(), _piles.end(), _piles[_curPileIdx], [](const ExtrusionLayer &l, const ExtrusionLayer &r) { return l.bottom_z < r.bottom_z; });
        auto end = std::upper_bound(_piles.begin(), _piles.end(), _piles[_curPileIdx], [](const ExtrusionLayer &l, const ExtrusionLayer &r) { return l.bottom_z < r.bottom_z; });
        return std::make_pair<int, int>(std::distance(_piles.begin(), begin), std::distance(_piles.begin(), end));
    }
    bool valid() const { return _curPileIdx < _piles.size(); }
    void raise()
    {
        if (!valid()) { return; }
        auto [b, e] = curRange();
        _curPileIdx += (e - b);
        _curBottomZ = _curPileIdx == _piles.size() ? _piles.back().bottom_z : _piles[_curPileIdx].bottom_z;
    }
    float curBottomZ() const { return _curBottomZ; }
    LineWithIDs curLines() const
    {
        auto [b, e] = curRange();
        LineWithIDs lines;
        for (int i = b; i < e; ++i) {
            for (const ExtrusionPath &path : _piles[i].paths) {
                if (path.is_force_no_extrusion() == false) {
                    Polyline check_polyline = path.polyline;
                    check_polyline.translate(_offset);
                    Lines tmpLines = check_polyline.lines();
                    for (const Line &line : tmpLines) { lines.emplace_back(line, _id, path.role()); }
                }
            }
        }
        return lines;
    }

    friend bool operator>(const LinesBucket &left, const LinesBucket &right) { return left._curBottomZ > right._curBottomZ; }
    friend bool operator<(const LinesBucket &left, const LinesBucket &right) { return left._curBottomZ < right._curBottomZ; }
    friend bool operator==(const LinesBucket &left, const LinesBucket &right) { return left._curBottomZ == right._curBottomZ; }
};

struct LinesBucketPtrComp
{
    bool operator()(const LinesBucket *left, const LinesBucket *right) { return *left > *right; }
};

class LinesBucketQueue
{
public:
    std::vector<LinesBucket>                                                           line_buckets;
    std::priority_queue<LinesBucket *, std::vector<LinesBucket *>, LinesBucketPtrComp> line_bucket_ptr_queue;

public:
    void        emplace_back_bucket(ExtrusionLayers &&els, const void *objPtr, Point offset);
    bool        valid() const { return line_bucket_ptr_queue.empty() == false; }
    float       getCurrBottomZ();
    LineWithIDs getCurLines() const;
};

void getExtrusionPathsFromEntity(const ExtrusionEntityCollection *entity, ExtrusionPaths &paths);

ExtrusionLayers getExtrusionPathsFromLayer(const LayerRegionPtrs layerRegionPtrs);

ExtrusionLayer getExtrusionPathsFromSupportLayer(SupportLayer *supportLayer);

ObjectExtrusions getAllLayersExtrusionPathsFromObject(PrintObject *obj);

struct ConflictComputeResult
{
    const void* _obj1;
    const void* _obj2;

    ConflictComputeResult(const void* o1, const void* o2) : _obj1(o1), _obj2(o2) {}
    ConflictComputeResult() = default;
};

using ConflictComputeOpt = std::optional<ConflictComputeResult>;

using ConflictObjName = std::optional<std::pair<std::string, std::string>>;

struct ConflictChecker
{
    static ConflictResultOpt  find_inter_of_lines_in_diff_objs(PrintObjectPtrs objs, std::optional<const FakeWipeTower *> wtdptr);
    static ConflictComputeOpt find_inter_of_lines(const LineWithIDs &lines);
    static ConflictComputeOpt line_intersect(const LineWithID &l1, const LineWithID &l2);
};

} // namespace Slic3r

#endif
