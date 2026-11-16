#include "GLGizmoAlignment.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {

// Maps world coordinates into the frame whose axes are the columns of basis.
Transform3d world_to_frame(const Matrix3d &basis)
{
    Transform3d trafo = Transform3d::Identity();
    trafo.linear()    = basis.transpose();
    return trafo;
}

// Model parts of an instance, as ModelObject::instance_bounding_box() or instance_convex_hull_bounding_box() takes them.
BoundingBoxf3 instance_box_in_frame(const ModelObject &object, const ModelInstance &instance, const Transform3d &to_frame, bool convex_hull)
{
    BoundingBoxf3 box;
    const Transform3d inst_matrix = to_frame * instance.get_transformation().get_matrix();
    for (const ModelVolume *v : object.volumes) {
        if (!v->is_model_part())
            continue;
        const Transform3d matrix = inst_matrix * v->get_matrix();
        box.merge(convex_hull ? v->get_convex_hull().transformed_bounding_box(matrix) : v->mesh().transformed_bounding_box(matrix));
    }
    return box;
}

BoundingBoxf3 volume_box_in_frame(const GLVolume &volume, const Matrix3d &basis)
{
    return volume.transformed_convex_hull_bounding_box(world_to_frame(basis) * volume.world_matrix());
}

} // namespace

GLGizmoAlignment::GLGizmoAlignment(GLCanvas3D& canvas) : m_canvas(canvas)
{
}

bool GLGizmoAlignment::align_objects(AlignType type, bool align_parent)
{
    if (!can_align(type, align_parent)) {
        return false;
    }

    switch (type) {
        case AlignType::CENTER_X: return align_to_center(type, align_parent);
        case AlignType::CENTER_Y: return align_to_center(type, align_parent);
        case AlignType::CENTER_Z: return align_to_center(type, align_parent);
        case AlignType::Y_MAX: return align_to_y_max(align_parent);
        case AlignType::Y_MIN: return align_to_y_min(align_parent);
        case AlignType::X_MAX: return align_to_x_max(align_parent);
        case AlignType::X_MIN: return align_to_x_min(align_parent);
        case AlignType::Z_MAX: return align_to_z_max(align_parent);
        case AlignType::Z_MIN: return align_to_z_min(align_parent);
        default:
            return false;
    }
}

bool GLGizmoAlignment::distribute_objects(AlignType type)
{
    if (!can_distribute(type)) {
        return false;
    }

    switch (type) {
    case AlignType::DISTRIBUTE_X:
        return distribute_x();
    case AlignType::DISTRIBUTE_Y:
        return distribute_y();
    case AlignType::DISTRIBUTE_Z:
        return distribute_z();
    default:
            return false;
    }
}

bool GLGizmoAlignment::align_to_y_max(bool align_parent)
{
    return align_objects_generic(
        [](const ObjectInfo& obj) { return obj.bbox.max.y(); },
        [](Vec3d& displacement, double target, double current_max) {
            displacement.y() = target - current_max;
        }, "Align Y Max", align_parent
    );
}

bool GLGizmoAlignment::align_to_y_min(bool align_parent)
{
    return align_objects_generic(
        [](const ObjectInfo& obj) { return obj.bbox.min.y(); },
        [](Vec3d& displacement, double target, double current_min) {
            displacement.y() = target - current_min;
        }, "Align Y Min", align_parent
    );
}

bool GLGizmoAlignment::align_to_x_max(bool align_parent)
{
    return align_objects_generic(
        [](const ObjectInfo& obj) { return obj.bbox.max.x(); },
        [](Vec3d& displacement, double target, double current_max) {
            displacement.x() = target - current_max;
        }, "Align X Max", align_parent
    );
}

bool GLGizmoAlignment::align_to_x_min(bool align_parent)
{
    return align_objects_generic(
        [](const ObjectInfo& obj) { return obj.bbox.min.x(); },
        [](Vec3d& displacement, double target, double current_min) {
            displacement.x() = target - current_min;
        }, "Align X Min", align_parent
    );
}

bool GLGizmoAlignment::align_to_center(AlignType type,bool align_parent)
{
    std::string type_str = type == AlignType::CENTER_X ? "X" : (type == AlignType::CENTER_Y ? "Y" : "Z");
    std::string      operation_name   = type == AlignType::CENTER_X ? "X Center" : (type == AlignType::CENTER_Y ? "Y Center" : "Z Center");
    const Matrix3d   basis            = reference_basis(align_parent);
    double           selection_center = 0.f;
    if (align_parent) {
        BoundingBoxf3 parent_box = get_parent_box(basis);
        selection_center         = get_current_coord(operation_name, parent_box);

        // Handle multiple volume selection
        if (is_parts_selection(true)) {
            if (!take_snapshot("Align to parent center " + type_str)) {
                return false;
            }

            get_selection().setup_cache();

            BoundingBoxf3 selection_bbox = get_items_box(basis);
            double        current_coord  = get_current_coord(operation_name, selection_bbox);
            Vec3d         displacement   = generate_displacement(operation_name, selection_center - current_coord);
            // Move the selected volumes together so that their collective bounding box is centered in the parent
            for (const GLVolume *volume : get_selected_part_volumes()) {
                get_selection().translate(volume->object_idx(), volume->instance_idx(), volume->volume_idx(), basis * displacement, false);
            }
            finish_operation(true);
            return true;
        }
        BoundingBoxf3 big_bb;
        auto          objects = get_selected_objects_info(big_bb, basis);
        if (objects.empty()) return false;
        // Handle multiple objects or parts selection

        if (!take_snapshot("Align to parent center " + type_str)) {
            return false;
        }

        get_selection().setup_cache();
        double current_coord = get_current_coord(operation_name, big_bb);
        Vec3d  displacement  = generate_displacement(operation_name, selection_center - current_coord);
        // Multiple objects: use selection bounding box for alignment
        for (const auto &obj : objects) {
            apply_transformation(obj.object_idx, obj.instance_idx, basis * displacement);
        }

        finish_operation();
        return true;
    }
    // Handle the parts of a single instance
    if (is_parts_selection(false)) {
        if (!take_snapshot("Align Parts Center " + type_str)){
            return false;
        }

        get_selection().setup_cache();

        BoundingBoxf3 selection_bbox = get_items_box(basis);
        selection_center                    = get_current_coord(operation_name, selection_bbox);

        // Move each part to align to the center of their collective bounding box
        for (const GLVolume *volume : get_selected_part_volumes()) {
            auto temp_box = volume_box_in_frame(*volume, basis);
            double current  = get_current_coord(operation_name, temp_box);
            double offset = selection_center - current;
            Vec3d displacement = generate_displacement(operation_name, offset);
            get_selection().translate(volume->object_idx(), volume->instance_idx(), volume->volume_idx(), basis * displacement, false);
        }

        finish_operation(true);
        return true;
    }
    BoundingBoxf3 big_bb;
    auto          objects = get_selected_objects_info(big_bb, basis);
    if (objects.empty()) return false;
    // Handle multiple instances
    selection_center = get_current_coord(operation_name, big_bb);

    if (!take_snapshot("Align Objects Center" + type_str)) {
        return false;
    }

    get_selection().setup_cache();
    // Multiple objects: use selection bounding box for alignment
    for (const auto& obj : objects) {
        BoundingBoxf3 temp_bbox     = obj.bbox;
        double        current_coord = get_current_coord(operation_name, temp_bbox);
        Vec3d  displacement  = generate_displacement(operation_name, selection_center - current_coord);
        apply_transformation(obj.object_idx, obj.instance_idx, basis * displacement);
    }
    finish_operation();
    return true;
}

bool GLGizmoAlignment::align_to_z_max(bool align_parent)
{
    return align_objects_generic(
        [](const ObjectInfo& obj) { return obj.bbox.max.z(); },
        [](Vec3d& displacement, double target, double current_max) {
            displacement.z() = target - current_max;
        }, "Align Z Max", align_parent
    );
}

bool GLGizmoAlignment::align_to_z_min(bool align_parent)
{
    return align_objects_generic(
        [](const ObjectInfo& obj) { return obj.bbox.min.z(); },
        [](Vec3d& displacement, double target, double current_min) {
            displacement.z() = target - current_min;
        }, "Align Z Min", align_parent
    );
}

bool GLGizmoAlignment::distribute_y()
{
    return distribute_objects_generic(
        [](const ObjectInfo& obj) { return obj.center.y(); },
        1,
        "Distribute Y"
    );
}

bool GLGizmoAlignment::distribute_x()
{
    return distribute_objects_generic(
        [](const ObjectInfo& obj) { return obj.center.x(); },
        0,
        "Distribute X"
    );
}

bool GLGizmoAlignment::distribute_z()
{
    return distribute_objects_generic(
        [](const ObjectInfo& obj) { return obj.center.z(); },
        2,
        "Distribute Z"
    );
}

template<typename GetCoordFunc, typename SetCoordFunc>
bool GLGizmoAlignment::align_objects_generic(GetCoordFunc get_coord, SetCoordFunc set_coord, const std::string &operation_name ,bool align_parent)
{
    const Matrix3d basis = reference_basis(align_parent);
    if (align_parent) {
        // Find reference coordinate
        BoundingBoxf3 parent_box      = get_parent_box(basis);
        double        reference_coord = get_current_coord(operation_name, parent_box);
        // Handle parts selection differently
        if (is_parts_selection(true)) {
            if (!take_snapshot("align to parent node " + operation_name)) {
                return false;
            }

            get_selection().setup_cache();

            const std::vector<const GLVolume *> volumes = get_selected_part_volumes();
            if (volumes.empty())
                return false;
            BoundingBoxf3 selection_bbox = get_items_box(basis);
            double        current_coord  = get_current_coord(operation_name, selection_bbox);
            Vec3d  displacement  = generate_displacement(operation_name, reference_coord - current_coord);
            for (const GLVolume *volume : volumes) {
                if (displacement.norm() > 1e-6) {
                    get_selection().translate(volume->object_idx(), volume->instance_idx(), volume->volume_idx(), basis * displacement, false);
                }
            }

            finish_operation(true);
            return true;
        }

        // Handle objects selection (original logic)
        BoundingBoxf3 big_bb;
        auto          objects = get_selected_objects_info(big_bb, basis);
        if (objects.empty()) return false;

        if (!take_snapshot("align to parent node " + operation_name)) { return false; }

        get_selection().setup_cache();

        double current_coord = get_current_coord(operation_name, big_bb);
        Vec3d  displacement  = generate_displacement(operation_name, reference_coord - current_coord);

        for (const auto &obj : objects) {
            if (displacement.norm() > 1e-6) {
                apply_transformation(obj.object_idx, obj.instance_idx, basis * displacement);
            }
        }

        finish_operation();
        return true;
    }
    // Handle parts selection differently
    if (is_parts_selection(false)) {
        if (!take_snapshot(operation_name)) {
            return false;
        }

        get_selection().setup_cache();

        const std::vector<const GLVolume*> volumes = get_selected_part_volumes();
        if (volumes.empty()) return false;

        // Find reference coordinate
        double reference_coord = 0.0;
        bool first = true;

        for (const GLVolume* volume : volumes) {
            BoundingBoxf3 bbox = volume_box_in_frame(*volume, basis);
            double coord = 0.0;

            if (operation_name.find("X Max") != std::string::npos) {
                coord = bbox.max.x();
            } else if (operation_name.find("X Min") != std::string::npos) {
                coord = bbox.min.x();
            } else if (operation_name.find("Y Max") != std::string::npos) {
                coord = bbox.max.y();
            } else if (operation_name.find("Y Min") != std::string::npos) {
                coord = bbox.min.y();
            } else if (operation_name.find("Z Max") != std::string::npos) {
                coord = bbox.max.z();
            } else if (operation_name.find("Z Min") != std::string::npos) {
                coord = bbox.min.z();
            }

            if (first) {
                reference_coord = coord;
                first = false;
            } else {
                if (operation_name.find("Max") != std::string::npos) {
                    reference_coord = std::max(reference_coord, coord);
                } else if (operation_name.find("Min") != std::string::npos) {
                    reference_coord = std::min(reference_coord, coord);
                }
            }
        }

        for (const GLVolume* volume : volumes) {
            BoundingBoxf3 bbox = volume_box_in_frame(*volume, basis);
            double current_coord = 0.0;

            if (operation_name.find("X Max") != std::string::npos) {
                current_coord = bbox.max.x();
            } else if (operation_name.find("X Min") != std::string::npos) {
                current_coord = bbox.min.x();
            } else if (operation_name.find("Y Max") != std::string::npos) {
                current_coord = bbox.max.y();
            } else if (operation_name.find("Y Min") != std::string::npos) {
                current_coord = bbox.min.y();
            } else if (operation_name.find("Z Max") != std::string::npos) {
                current_coord = bbox.max.z();
            } else if (operation_name.find("Z Min") != std::string::npos) {
                current_coord = bbox.min.z();
            }

            Vec3d displacement = Vec3d::Zero();

            if (operation_name.find("X") != std::string::npos) {
                displacement.x() = reference_coord - current_coord;
            } else if (operation_name.find("Y") != std::string::npos) {
                displacement.y() = reference_coord - current_coord;
            } else if (operation_name.find("Z") != std::string::npos) {
                displacement.z() = reference_coord - current_coord;
            }

            if (displacement.norm() > 1e-6) {
                get_selection().translate(volume->object_idx(), volume->instance_idx(), volume->volume_idx(), basis * displacement, false);
            }
        }

        finish_operation(true);
        return true;
    }

    // Handle objects selection (original logic)
    BoundingBoxf3 big_bb;
    auto          objects = get_selected_objects_info(big_bb, basis);
    if (objects.empty()) return false;

    if (!take_snapshot(operation_name)) {
        return false;
    }

    get_selection().setup_cache();

    double reference_coord = get_coord(objects[0]);
    for (const auto& obj : objects) {
        double coord = get_coord(obj);
        if (operation_name.find("Max") != std::string::npos) {
            reference_coord = std::max(reference_coord, coord);
        } else if (operation_name.find("Min") != std::string::npos) {
            reference_coord = std::min(reference_coord, coord);
        }
    }

    for (const auto& obj : objects) {
        double current_coord = get_coord(obj);
        Vec3d displacement = Vec3d::Zero();
        set_coord(displacement, reference_coord, current_coord);

        if (displacement.norm() > 1e-6) {
            apply_transformation(obj.object_idx, obj.instance_idx, basis * displacement);
        }
    }

    finish_operation();
    return true;
}

template<typename GetCoordFunc>
bool GLGizmoAlignment::distribute_objects_generic(GetCoordFunc get_coord, int axis, const std::string& operation_name)
{
    const Matrix3d basis = reference_basis(false);
    if (is_parts_selection(false)) {
        const std::vector<const GLVolume*> volumes = get_selected_part_volumes();
        if (volumes.size() < 3) return false;
        if (!take_snapshot(operation_name)) {
            return false;
        }
        get_selection().setup_cache();

        struct VolEntry { const GLVolume* v; double coord; };
        std::vector<VolEntry> entries;
        entries.reserve(volumes.size());
        for (const GLVolume* v : volumes) {
            BoundingBoxf3 bbox = volume_box_in_frame(*v, basis);
            Vec3d c = bbox.center();
            double coord = (axis == 0 ? c.x() : axis == 1 ? c.y() : c.z());
            entries.push_back({v, coord});
        }

        std::sort(entries.begin(), entries.end(), [](const VolEntry& a, const VolEntry& b){ return a.coord < b.coord; });

        BoundingBoxf3 selection_bbox = get_items_box(basis);
        BoundingBoxf3 left_bbox      = volume_box_in_frame(*entries.front().v, basis);
        BoundingBoxf3 right_bbox     = volume_box_in_frame(*entries.back().v, basis);
        auto   axis_extent = [&](const BoundingBoxf3 &bb) { return (axis == 0 ? (bb.max.x() - bb.min.x()) : axis == 1 ? (bb.max.y() - bb.min.y()) : (bb.max.z() - bb.min.z())); };
        double left_width  = axis_extent(left_bbox);
        double right_width = axis_extent(right_bbox);
        double bbox_min    = (axis == 0 ? selection_bbox.min.x() : axis == 1 ? selection_bbox.min.y() : selection_bbox.min.z());
        double bbox_max    = (axis == 0 ? selection_bbox.max.x() : axis == 1 ? selection_bbox.max.y() : selection_bbox.max.z());
        double min_center  = bbox_min + left_width * 0.5;
        double max_center  = bbox_max - right_width * 0.5;
        double total_distance = max_center - min_center;
        double interval       = (entries.size() > 1) ? total_distance / (entries.size() - 1) : 0;
        for (size_t i = 1; i < entries.size() - 1; ++i) {
            double target_coord  = min_center + i * interval;
            double current_coord = entries[i].coord;
            double offset        = target_coord - current_coord;

            if (std::abs(offset) > 1e-6) {
                Vec3d displacement = Vec3d::Zero();
                displacement[axis] = offset;
                const GLVolume *v  = entries[i].v;
                get_selection().translate(v->object_idx(), v->instance_idx(), v->volume_idx(), basis * displacement, false);
            }
        }

        finish_operation(true);
        return true;
    }
    BoundingBoxf3 big_bb;
    auto          objects = get_selected_objects_info(big_bb, basis);
    if (objects.size() < 3) return false;

    if (!take_snapshot(operation_name)) {
        return false;
    }

    get_selection().setup_cache();

    std::sort(objects.begin(), objects.end(),
        [&get_coord](const ObjectInfo& a, const ObjectInfo& b) {
            return get_coord(a) < get_coord(b);
        });

    double min_coord = get_coord(objects.front());
    double max_coord = get_coord(objects.back());
    double total_distance = max_coord - min_coord;
    double interval = (objects.size() > 1) ? total_distance / (objects.size() - 1) : 0;
    for (size_t i = 1; i < objects.size() - 1; ++i) {
        double target_coord = min_coord + i * interval;
        double current_coord = get_coord(objects[i]);
        double offset = target_coord - current_coord;

        if (std::abs(offset) > 1e-6) {
            Vec3d displacement = Vec3d::Zero();
            displacement[axis] = offset;
            apply_transformation(objects[i].object_idx, objects[i].instance_idx, basis * displacement);
        }
    }

    finish_operation();
    return true;
}

bool GLGizmoAlignment::can_align(AlignType type, bool align_parent) const
{
    return get_items_count(type, align_parent) >= (align_parent ? 1 : 2);
}

bool GLGizmoAlignment::can_distribute(AlignType type) const
{
    return (type == AlignType::DISTRIBUTE_X || type == AlignType::DISTRIBUTE_Y || type == AlignType::DISTRIBUTE_Z) &&
           get_items_count(type, false) >= 3;
}

double GLGizmoAlignment::get_current_coord(std::string operation_name, BoundingBoxf3 &bbox)
{
    double current_coord = 0.0;
    if (operation_name.find("X Max") != std::string::npos) {
        current_coord = bbox.max.x();
    } else if (operation_name.find("X Min") != std::string::npos) {
        current_coord = bbox.min.x();
    } else if (operation_name.find("Y Max") != std::string::npos) {
        current_coord = bbox.max.y();
    } else if (operation_name.find("Y Min") != std::string::npos) {
        current_coord = bbox.min.y();
    } else if (operation_name.find("Z Max") != std::string::npos) {
        current_coord = bbox.max.z();
    } else if (operation_name.find("Z Min") != std::string::npos) {
        current_coord = bbox.min.z();
    } else if (operation_name.find("X Center") != std::string::npos) {
        current_coord = bbox.center().x();
    } else if (operation_name.find("Y Center") != std::string::npos) {
        current_coord = bbox.center().y();
    } else if (operation_name.find("Z Center") != std::string::npos) {
        current_coord = bbox.center().z();
    }
    return current_coord;
}

Vec3d GLGizmoAlignment::generate_displacement(std::string operation_name, double offset) {
    Vec3d displacement = Vec3d::Zero();
    if (operation_name.find("X") != std::string::npos) {
        displacement.x() = offset;
    } else if (operation_name.find("Y") != std::string::npos) {
        displacement.y() = offset;
    } else if (operation_name.find("Z") != std::string::npos) {
        displacement.z() = offset;
    }
    return displacement;
}

Matrix3d GLGizmoAlignment::reference_basis(bool align_parent) const
{
    // Plate alignment stays on the world axes.
    const Selection &selection = get_selection();
    if (!m_object_cs || selection.is_empty() || (align_parent && !is_parts_selection(true)))
        return Matrix3d::Identity();

    // The instance of the first selected volume, as the Move gizmo's object coordinate system does.
    auto transformation = selection.get_first_volume()->get_instance_transformation();
    transformation.reset_scaling_factor();
    return transformation.get_matrix_no_offset().linear();
}

std::vector<GLGizmoAlignment::ObjectInfo> GLGizmoAlignment::get_selected_objects_info(BoundingBoxf3 &big_bb, const Matrix3d &basis) const
{
    std::vector<ObjectInfo> objects;
    Selection& selection = get_selection();

    if (selection.is_empty()) return objects;

    const Transform3d to_frame = world_to_frame(basis);
    const auto& content = selection.get_content();
    for (const auto& obj_it : content) {
        for (const auto& inst_idx : obj_it.second) {
            int obj_idx = obj_it.first;

            if (obj_idx >= 0 && obj_idx < selection.get_model()->objects.size()) {
                ModelObject* object = selection.get_model()->objects[obj_idx];
                if (inst_idx >= 0 && inst_idx < object->instances.size()) {
                    BoundingBoxf3 bbox = instance_box_in_frame(*object, *object->instances[inst_idx], to_frame, false);
                    big_bb.merge(bbox);
                    objects.emplace_back(obj_idx, inst_idx, bbox);
                }
            }
        }
    }

    return objects;
}

bool GLGizmoAlignment::is_parts_selection(bool align_parent) const
{
    const Selection &selection = get_selection();
    const auto      &content   = selection.get_content();
    if (content.size() != 1 || content.begin()->first >= 1000 || content.begin()->second.size() != 1)
        return false;
    return !align_parent || selection.get_mode() == Selection::Volume;
}

std::vector<const GLVolume *> GLGizmoAlignment::get_selected_part_volumes() const
{
    std::vector<const GLVolume *> volumes;
    if (!is_parts_selection(false))
        return volumes;

    const Selection   &selection = get_selection();
    const ModelObject *object    = selection.get_model()->objects[selection.get_object_idx()];
    if (object->is_cut())
        return volumes;

    for (unsigned int idx : selection.get_volume_idxs()) {
        const GLVolume *volume = selection.get_volume(idx);
        if (volume->volume_idx() < 0)
            continue;
        // Moving a whole instance's parts leaves its modifiers, negative volumes and support blockers in place,
        // as dragging a part does.
        if (selection.get_mode() == Selection::Volume || object->volumes[volume->volume_idx()]->is_model_part())
            volumes.push_back(volume);
    }
    return volumes;
}

BoundingBoxf3 GLGizmoAlignment::get_items_box(const Matrix3d &basis) const
{
    BoundingBoxf3 box;
    if (is_parts_selection(false)) {
        for (const GLVolume *volume : get_selected_part_volumes())
            box.merge(volume_box_in_frame(*volume, basis));
    } else
        get_selected_objects_info(box, basis);
    return box;
}

BoundingBoxf3 GLGizmoAlignment::get_parent_box(const Matrix3d &basis) const
{
    const Selection &selection = get_selection();
    if (is_parts_selection(true)) {
        const ModelObject *object = selection.get_model()->objects[selection.get_object_idx()];
        return instance_box_in_frame(*object, *object->instances[selection.get_instance_idx()], world_to_frame(basis), true);
    }

    // can_align() requires every selected instance on one plate, so the first one gives the plate.
    BoundingBoxf3 box;
    PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
    for (const auto &[obj_idx, inst_idxs] : selection.get_content()) {
        if (obj_idx >= 1000 || inst_idxs.empty())
            continue;
        PartPlate *plate = plate_list.get_plate(plate_list.find_instance(obj_idx, *inst_idxs.begin()));
        if (plate == nullptr)
            break;
        BoundingBoxf3 instances_box;
        get_selected_objects_info(instances_box, Matrix3d::Identity());
        box          = plate->get_plate_box();
        box.max.z()  = instances_box.max.z();
        // Keep the aligned objects off the plate edge.
        box.min     += Vec3d(0.1, 0.1, 0.1);
        box.max     -= Vec3d(0.1, 0.1, 0.1);
        break;
    }
    return box;
}

Selection& GLGizmoAlignment::get_selection() const
{
    return m_canvas.get_selection();
}

bool GLGizmoAlignment::take_snapshot(const std::string& name)
{
    wxGetApp().plater()->take_snapshot(name);
    return true;
}

void GLGizmoAlignment::apply_transformation(int obj_idx, int inst_idx, const Vec3d& displacement)
{
    get_selection().translate(obj_idx, inst_idx, displacement);
}

void GLGizmoAlignment::finish_operation(bool force_volume_move)
{
    // A part move changes the ModelVolume shared by every copy of the object, so the other copies' volumes must show
    // it too, or do_move() would write their stale transformation back to the model.
    if (force_volume_move)
        get_selection().synchronize_unselected_volumes();
    // The operation already took its snapshot.
    m_canvas.do_move("", force_volume_move);
}

size_t GLGizmoAlignment::get_items_count(AlignType type, bool align_parent, std::string *why_not) const
{
    auto cannot_move = [why_not](const std::string &reason) {
        if (why_not != nullptr)
            *why_not = reason;
        return size_t(0);
    };

    if (is_parts_selection(align_parent)) {
        const size_t count = get_selected_part_volumes().size();
        return count > 0 ? count : cannot_move(_u8L("Parts of a cut object cannot be moved"));
    }

    // Instances are moved through their instance offsets, which do_move() commits only from an Instance selection.
    const Selection &selection = get_selection();
    if (selection.get_mode() != Selection::Instance)
        return 0;

    const bool is_z = type == AlignType::Z_MIN || type == AlignType::CENTER_Z || type == AlignType::Z_MAX ||
                      type == AlignType::DISTRIBUTE_Z;
    // The plate gives no Z target, and aligning to its bottom would duplicate Drop.
    if (is_z && align_parent)
        return cannot_move(_u8L("Aligning to the plate is available in X and Y only"));

    PartPlateList &plate_list = wxGetApp().plater()->get_partplate_list();
    int            plate_idx  = -1;
    size_t         count      = 0;
    for (const auto &[obj_idx, inst_idxs] : selection.get_content()) {
        if (obj_idx >= 1000)
            continue;
        // Auto-drop would put a raised object back on the bed and leave a lowered one sunk.
        if (is_z && selection.get_model()->objects[obj_idx]->auto_drop)
            return cannot_move(_u8L("Turn off Auto-Drop for the selected objects to move them in Z"));
        if (align_parent) {
            // The plate holding the selection is the target, so it must hold all of it.
            for (int inst_idx : inst_idxs) {
                const int idx = plate_list.find_instance(obj_idx, inst_idx);
                if (idx == -1 || (plate_idx != -1 && idx != plate_idx))
                    return cannot_move(_u8L("All selected objects must be on the same plate"));
                plate_idx = idx;
            }
        }
        count += inst_idxs.size();
    }
    return count;
}

} // namespace GUI
} // namespace Slic3r
