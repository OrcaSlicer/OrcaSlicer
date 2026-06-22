#pragma once

#include "Result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace orca {

class Session;

using ExportHandle = std::uint64_t;

struct ExportParams {
    std::filesystem::path output_path;
    int                   plate_id        = 0;
    bool                  send_to_printer = false;
};

// Class is `Exporter` (not `Export`) because `export` is a reserved word in C++.
class Exporter {
public:
    ~Exporter();

    Exporter(const Exporter&)            = delete;
    Exporter& operator=(const Exporter&) = delete;
    Exporter(Exporter&&)                 = delete;
    Exporter& operator=(Exporter&&)      = delete;

    Result<ExportHandle> export_gcode(ExportParams params);
    void                 cancel(ExportHandle handle);

    bool is_busy() const;

private:
    friend class Session;
    Exporter();

    // Set by Session::create() so export_gcode() can reach slicer().
    void bind_session(Session* session);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace orca
