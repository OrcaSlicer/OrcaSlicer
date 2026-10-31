#pragma once
#include <functional>

namespace Slic3r { namespace tex2color {

struct AlgoProgress {
    int percent = 0;
    const char* message = "";
};

using AlgoProgressCallback = std::function<void(AlgoProgress)>;
using AlgoCancelCallback   = std::function<bool()>;

} // namespace tex2color
} // namespace Slic3r
