#pragma once

#include <string>
#include <optional>

namespace Slic3r {

struct ConflictResult
{
    std::string        _objName1;
    std::string        _objName2;
    double             _height{ 0.0 };
    const void *       _obj1{ nullptr }; // nullptr means wipe tower
    const void *       _obj2{ nullptr };
    int                layer = -1;
    ConflictResult(const std::string &objName1, const std::string &objName2, double height, const void *obj1, const void *obj2)
        : _objName1(objName1), _objName2(objName2), _height(height), _obj1(obj1), _obj2(obj2)
    {}
    ConflictResult() = default;
};

using ConflictResultOpt = std::optional<ConflictResult>;

} // namespace Slic3r
