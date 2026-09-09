#include "OrcaModelPreparation.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Slic3r::GUI {

ModelPreparation prepare_model(const ModelObject& source, const ModelPreparationOptions& options)
{
    const double height = options.total_height_mm;
    const double base_height = options.base_height_mm;
    if (!std::isfinite(height) || height < 1.0 || height > 1000.0 ||
        (options.add_round_base && (!std::isfinite(base_height) || base_height < 0.5 || base_height >= height)))
        throw std::invalid_argument("invalid_dimensions");
    if (source.instances.size() != 1 || source.is_cut())
        throw std::invalid_argument("requires_single_uncut_instance");
    const ModelVolume* first_part = nullptr;
    for (const auto* volume : source.volumes) {
        if (volume->is_model_part() && first_part == nullptr)
            first_part = volume;
        if (options.add_round_base && volume->name == "AI round base")
            throw std::invalid_argument("base_already_exists");
    }
    if (first_part == nullptr)
        throw std::invalid_argument("empty_model");

    Transform3d matrix = source.instances.front()->get_matrix();
    if (!matrix.matrix().allFinite() || std::abs(matrix.linear().determinant()) < 1e-12)
        throw std::invalid_argument("invalid_transform");
    auto bounds = source.instance_bounding_box(0);
    if (!bounds.defined || !bounds.size().allFinite() || bounds.size().z() <= 1e-6)
        throw std::invalid_argument("empty_model");
    const Vec3d original_center = bounds.center();

    const double overlap = options.add_round_base ? std::min(0.2, base_height / 2.0) : 0.0;
    const double body_height = height - (options.add_round_base ? base_height - overlap : 0.0);
    matrix.linear() *= body_height / bounds.size().z();
    bounds = BoundingBoxf3();
    for (const auto* volume : source.volumes)
        if (volume->is_model_part())
            bounds.merge(volume->mesh().transformed_bounding_box(matrix * volume->get_matrix()));
    matrix.translation() += Vec3d(
        original_center.x() - bounds.center().x(), original_center.y() - bounds.center().y(),
        (options.add_round_base ? base_height - overlap : 0.0) - bounds.min.z());

    ModelPreparation result;
    result.instance = Geometry::Transformation(matrix);

    if (options.add_round_base) {
        // Cover the complete XY footprint, with a 2 mm radial margin. The small
        // overlap joins the base to the body during native multipart slicing.
        const double radius = std::hypot(bounds.size().x(), bounds.size().y()) / 2.0 + 2.0;
        TriangleMesh mesh(its_make_cylinder(radius, base_height));
        result.base = std::make_unique<Model>();
        auto* base = result.base->add_object()->add_volume(std::move(mesh), ModelVolumeType::MODEL_PART, false);
        base->name = "AI round base";
        base->config.set_key_value("extruder", new ConfigOptionInt(std::max(1, first_part->extruder_id())));
        Transform3d world = Transform3d::Identity();
        world.translation() = Vec3d(original_center.x(), original_center.y(), 0.0);
        base->set_transformation(matrix.inverse() * world);
    }
    return result;
}

void apply_model_preparation(ModelObject& source, const ModelPreparation& proposal)
{
    source.instances.front()->set_transformation(proposal.instance);
    if (proposal.base)
        source.add_volume(*proposal.base->objects.front()->volumes.front());
    source.invalidate_bounding_box();
}

} // namespace Slic3r::GUI
