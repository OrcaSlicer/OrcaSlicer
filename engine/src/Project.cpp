// Project — engine-side ModelService.
//
// Phase 0.4a only needs Project to be a constructible no-op so Session can
// own it. The real implementation lands in Phase 0.4b (ModelService rerouting).

#include "orca/Project.hpp"

#include "orca/Events.hpp"
#include "orca/EventTypes.hpp"
#include "orca/Session.hpp"

#include "libslic3r/Model.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>

namespace orca {

struct Project::Impl {
    Slic3r::Model* model   = nullptr;  // borrowed; not owned
    Session*       session = nullptr;  // back-pointer for event publishing
};

Project::Project() : impl_(std::make_unique<Impl>()) {}
Project::~Project() = default;

void Project::bind_session(Session* session) {
    impl_->session = session;
}

void Project::attach_model(Slic3r::Model* model) {
    Slic3r::Model* old_model = impl_->model;

    // Mirror "everything previously here is now gone" before the borrow flips.
    if (old_model != nullptr && impl_->session != nullptr) {
        for (std::size_t i = 0; i < old_model->objects.size(); ++i) {
            impl_->session->events().publish<ObjectRemoved>(
                ObjectRemoved{static_cast<ObjectId>(i)});
        }
    }

    impl_->model = model;

    if (model != nullptr && impl_->session != nullptr) {
        // Phase 1: borrow path has no LoadHandle and no source path. Phase 2
        // load_files() will publish ProjectLoaded with the real handle+path.
        impl_->session->events().publish<ProjectLoaded>(
            ProjectLoaded{LoadHandle{0}, std::filesystem::path{}});
        for (std::size_t i = 0; i < model->objects.size(); ++i) {
            impl_->session->events().publish<ObjectAdded>(
                ObjectAdded{static_cast<ObjectId>(i)});
        }
    }
}

void Project::detach_model() {
    Slic3r::Model* old_model = impl_->model;
    if (old_model != nullptr && impl_->session != nullptr) {
        for (std::size_t i = 0; i < old_model->objects.size(); ++i) {
            impl_->session->events().publish<ObjectRemoved>(
                ObjectRemoved{static_cast<ObjectId>(i)});
        }
    }
    impl_->model = nullptr;
}

bool Project::has_model() const noexcept {
    return impl_->model != nullptr;
}

Slic3r::Model& Project::raw() {
    assert(impl_->model && "Project::raw() called before Session::attach_model()");
    return *impl_->model;
}

const Slic3r::Model& Project::raw() const {
    assert(impl_->model && "Project::raw() called before Session::attach_model()");
    return *impl_->model;
}

Slic3r::Model* Project::raw_ptr() {
    return impl_->model;
}

const Slic3r::Model* Project::raw_ptr() const {
    return impl_->model;
}

Result<LoadHandle> Project::load_files(
    const std::vector<std::filesystem::path>& /*paths*/,
    const LoadOptions&                        /*options*/)
{
    return err<LoadHandle>(ErrorCode::NotImplemented, "Project::load_files — Phase 0.4b");
}

Result<void> Project::save(const std::filesystem::path& /*path*/) {
    return err<void>(ErrorCode::NotImplemented, "Project::save — Phase 0.4b");
}

std::size_t Project::object_count() const { return 0; }
std::vector<Object> Project::objects() { return {}; }
std::optional<Object> Project::object(ObjectId /*id*/) { return std::nullopt; }

Result<void> Project::remove_object(ObjectId /*id*/) {
    return err<void>(ErrorCode::NotImplemented, "Project::remove_object — Phase 0.4b");
}

Result<void> Project::clear() {
    return err<void>(ErrorCode::NotImplemented, "Project::clear — Phase 0.4b");
}

// Lightweight handle method definitions — out-of-line so the header doesn't
// need to include libslic3r's Model.hpp. All return defaults for the Phase
// 0.4a placeholder; the real bindings come in Phase 0.4b alongside the
// model attach plumbing.

InstanceId Instance::id() const { return 0; }
Transform  Instance::transform() const { return {}; }
Result<void> Instance::set_offset(double, double, double)         { return err<void>(ErrorCode::NotImplemented, "Instance::set_offset — Phase 0.4b"); }
Result<void> Instance::set_rotation(double, double, double)       { return err<void>(ErrorCode::NotImplemented, "Instance::set_rotation — Phase 0.4b"); }
Result<void> Instance::set_scaling_factor(double, double, double) { return err<void>(ErrorCode::NotImplemented, "Instance::set_scaling_factor — Phase 0.4b"); }
Result<void> Instance::set_mirror(double, double, double)         { return err<void>(ErrorCode::NotImplemented, "Instance::set_mirror — Phase 0.4b"); }

VolumeId    Volume::id() const { return 0; }
std::string Volume::name() const { return {}; }
VolumeType  Volume::type() const { return VolumeType::Model; }
Transform   Volume::transform() const { return {}; }
Result<void> Volume::set_name(std::string_view) { return err<void>(ErrorCode::NotImplemented, "Volume::set_name — Phase 0.4b"); }
Result<void> Volume::set_type(VolumeType)       { return err<void>(ErrorCode::NotImplemented, "Volume::set_type — Phase 0.4b"); }

ObjectId    Object::id() const { return 0; }
std::string Object::name() const { return {}; }
Result<void> Object::set_name(std::string_view) { return err<void>(ErrorCode::NotImplemented, "Object::set_name — Phase 0.4b"); }
std::size_t Object::instance_count() const { return 0; }
Instance    Object::instance(std::size_t) { return {}; }
std::vector<Instance> Object::instances() { return {}; }
std::size_t Object::volume_count() const { return 0; }
Volume      Object::volume(std::size_t) { return {}; }
std::vector<Volume>   Object::volumes() { return {}; }
Result<Volume> Object::add_volume(const std::filesystem::path&, VolumeType) {
    return err<Volume>(ErrorCode::NotImplemented, "Object::add_volume — Phase 0.4b");
}
Result<void> Object::remove_volume(VolumeId) {
    return err<void>(ErrorCode::NotImplemented, "Object::remove_volume — Phase 0.4b");
}

} // namespace orca
