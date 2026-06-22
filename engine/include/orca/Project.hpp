#pragma once

#include "Result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Forward declaration of the borrowed libslic3r Model — same migration-scaffold
// pattern used by Presets::raw() / raw_ptr() for PresetBundle. Phase 0.4b
// rewrites every `wxGetApp().plater()->model()` GUI site to
// `::orca::session().project().raw()`; Phase 1 collapses ownership into Project.
namespace Slic3r {
class Model;
} // namespace Slic3r

namespace orca {

class Session;

using ObjectId   = std::uint64_t;
using InstanceId = std::uint64_t;
using VolumeId   = std::uint64_t;
using LoadHandle = std::uint64_t;

enum class VolumeType {
    Model,
    NegativeVolume,
    ParameterModifier,
    SupportBlocker,
    SupportEnforcer,
};

// Mirrors libslic3r::LoadStrategy. Bitwise-or composes flags.
enum class LoadStrategy : std::uint32_t {
    Default             = 0,
    AddDefaultInstances = 1 << 0,
    CheckVersion        = 1 << 1,
    LoadModel           = 1 << 2,
    LoadConfig          = 1 << 3,
    LoadAuxiliary       = 1 << 4,
    Silent              = 1 << 5,
    ImperialUnits       = 1 << 6,
};

inline LoadStrategy operator|(LoadStrategy a, LoadStrategy b) {
    using U = std::underlying_type_t<LoadStrategy>;
    return static_cast<LoadStrategy>(static_cast<U>(a) | static_cast<U>(b));
}
inline LoadStrategy operator&(LoadStrategy a, LoadStrategy b) {
    using U = std::underlying_type_t<LoadStrategy>;
    return static_cast<LoadStrategy>(static_cast<U>(a) & static_cast<U>(b));
}
inline bool has(LoadStrategy flags, LoadStrategy flag) {
    using U = std::underlying_type_t<LoadStrategy>;
    return (static_cast<U>(flags) & static_cast<U>(flag)) != 0;
}

struct Transform {
    std::array<double, 3> offset           = {0.0, 0.0, 0.0};
    std::array<double, 3> rotation         = {0.0, 0.0, 0.0};
    std::array<double, 3> scaling_factor   = {1.0, 1.0, 1.0};
    std::array<double, 3> mirror           = {1.0, 1.0, 1.0};
};

struct LoadOptions {
    LoadStrategy strategy = LoadStrategy::AddDefaultInstances;
    int          plate_id = 0;
};

// ---------- Lightweight non-owning handles ----------
//
// Object/Instance/Volume are pointer-sized handles whose lifetime is tied to
// the owning Project. Copying them is cheap; they do not own the underlying
// model data. A handle becomes dangling if the underlying object is removed —
// the consumer is responsible for not holding stale handles across mutations.
//
// `native_` is an opaque void* to the libslic3r ModelObject/ModelInstance/
// ModelVolume (or nullptr for a default-constructed/invalid handle). The
// engine boundary is preserved: consumers cannot dereference native_.

class Instance {
public:
    Instance() = default;

    InstanceId   id() const;
    bool         valid() const noexcept { return native_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    Transform    transform() const;
    Result<void> set_offset(double x, double y, double z);
    Result<void> set_rotation(double rx, double ry, double rz);
    Result<void> set_scaling_factor(double sx, double sy, double sz);
    Result<void> set_mirror(double mx, double my, double mz);

private:
    friend class Object;
    explicit Instance(void* native) : native_(native) {}
    void* native_ = nullptr;
};

class Volume {
public:
    Volume() = default;

    VolumeId    id() const;
    bool        valid() const noexcept { return native_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    std::string  name() const;
    VolumeType   type() const;
    Transform    transform() const;
    Result<void> set_name(std::string_view name);
    Result<void> set_type(VolumeType type);

private:
    friend class Object;
    explicit Volume(void* native) : native_(native) {}
    void* native_ = nullptr;
};

class Object {
public:
    Object() = default;

    ObjectId    id() const;
    bool        valid() const noexcept { return native_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    std::string name() const;
    Result<void> set_name(std::string_view name);

    std::size_t           instance_count() const;
    Instance              instance(std::size_t idx);
    std::vector<Instance> instances();

    std::size_t           volume_count() const;
    Volume                volume(std::size_t idx);
    std::vector<Volume>   volumes();

    Result<Volume> add_volume(const std::filesystem::path& mesh_path, VolumeType type);
    Result<void>   remove_volume(VolumeId id);

private:
    friend class Project;
    explicit Object(void* native) : native_(native) {}
    void* native_ = nullptr;
};

class Project {
public:
    ~Project();

    Project(const Project&)            = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&)                 = delete;
    Project& operator=(Project&&)      = delete;

    Result<LoadHandle> load_files(
        const std::vector<std::filesystem::path>& paths,
        const LoadOptions&                        options = {});

    Result<void> save(const std::filesystem::path& path);

    std::size_t           object_count() const;
    std::vector<Object>   objects();
    std::optional<Object> object(ObjectId id);

    Result<void> remove_object(ObjectId id);
    Result<void> clear();

    // ---------- Migration scaffold (Phase 0.4b) ----------
    //
    // raw() returns the borrowed Slic3r::Model. Transform B rewrites every
    // `wxGetApp().plater()->model()` GUI call site to
    // `::orca::session().project().raw()` — direct semantic equivalence since
    // both forms produce a `Model&`. The model is owned by Plater (GUI) for
    // the migration window; Phase 1 moves ownership into Project. Asserts the
    // model is attached.
    Slic3r::Model&       raw();
    const Slic3r::Model& raw() const;
    Slic3r::Model*       raw_ptr();
    const Slic3r::Model* raw_ptr() const;
    bool                 has_model() const noexcept;

private:
    friend class Session;
    Project();

    void bind_session(Session* session);
    void attach_model(Slic3r::Model* model);
    void detach_model();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace orca
