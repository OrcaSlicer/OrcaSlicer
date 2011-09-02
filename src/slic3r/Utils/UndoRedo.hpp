#ifndef slic3r_Utils_UndoRedo_hpp_
#define slic3r_Utils_UndoRedo_hpp_

#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <cassert>

#include <libslic3r/ObjectID.hpp>
#include <libslic3r/Config.hpp>

typedef double                          coordf_t;
typedef std::pair<coordf_t, coordf_t>   t_layer_height_range;

namespace Slic3r {

class Model;

namespace GUI {
	class Selection;
    class GLGizmosManager;
	class PartPlateList;
	class PartPlate;
} // namespace GUI

namespace UndoRedo {

enum class SnapshotType : unsigned char {
	// Some action modifying project state, outside any EnteringGizmo / LeavingGizmo interval.
	Action,
	// Some action modifying project state, inside some EnteringGizmo / LeavingGizmo interval.
	GizmoAction,
	// Selection change at the Plater.
	Selection,
	// New project, Reset project, Load project ...
	ProjectSeparator,
	// Entering a Gizmo, which opens a secondary Undo / Redo stack.
	EnteringGizmo,
	// Leaving a Gizmo, which closes a secondary Undo / Redo stack. 
	// No action modifying a project state was done between EnteringGizmo / LeavingGizmo.
	LeavingGizmoNoAction,
	// Leaving a Gizmo, which closes a secondary Undo / Redo stack.
	// Some action modifying a project state was done between EnteringGizmo / LeavingGizmo.
	LeavingGizmoWithAction,
};

// Data structure to be stored with each snapshot.
// Storing short data (bit masks, ints) with each snapshot instead of being serialized into the Undo / Redo stack
// is likely cheaper in term of both the runtime and memory allocation.
// Also the SnapshotData is available without having to deserialize the snapshot from the Undo / Redo stack,
// which may be handy sometimes.
struct SnapshotData
{
	SnapshotType        snapshot_type;
	PrinterTechnology 	printer_technology { ptUnknown };
	// Bitmap of Flags (see the Flags enum).
	unsigned int        flags { 0 };
    int                 layer_range_idx { -1 };

	// Bitmask of various binary flags to be stored with the snapshot.
	enum Flags {
		VARIABLE_LAYER_EDITING_ACTIVE = 1,
		SELECTED_SETTINGS_ON_SIDEBAR  = 2,
		SELECTED_LAYERROOT_ON_SIDEBAR = 4,
		SELECTED_LAYER_ON_SIDEBAR     = 8,
        RECALCULATE_SLA_SUPPORTS      = 16
	};
};

struct Snapshot
{
	Snapshot(size_t timestamp) : timestamp(timestamp) {}
	Snapshot(const std::string &name, size_t timestamp, size_t model_id, const SnapshotData &snapshot_data) :
		name(name), timestamp(timestamp), model_id(model_id), snapshot_data(snapshot_data) {}
	
	std::string 		name;
	size_t 				timestamp;
	size_t 				model_id;
	SnapshotData  		snapshot_data;

	bool		operator< (const Snapshot &rhs) const { return this->timestamp < rhs.timestamp; }
	bool		operator==(const Snapshot &rhs) const { return this->timestamp == rhs.timestamp; }

	// The topmost snapshot represents the current state when going forward.
	bool 		is_topmost() const;
	// The topmost snapshot is not being serialized to the Undo / Redo stack until going back in time, 
	// when the top most state is being serialized, so we can redo back to the top most state.
	bool 		is_topmost_captured() const { assert(this->is_topmost()); return model_id > 0; }

};

// BBS: moved from UndoRedo.cpp
// If a snapshot modifies the snapshot type, 
inline bool snapshot_modifies_project(SnapshotType type)
{
	return type == SnapshotType::Action || type == SnapshotType::GizmoAction || type == SnapshotType::ProjectSeparator;
}

inline bool snapshot_modifies_project(const Snapshot &snapshot)
{
	return snapshot_modifies_project(snapshot.snapshot_data.snapshot_type) && (snapshot.name.empty() || snapshot.name.back() != '!');
}

// Excerpt of Slic3r::GUI::Selection for serialization onto the Undo / Redo stack.
struct Selection : public Slic3r::ObjectBase {
	void clear() { mode = 0; volumes_and_instances.clear(); }
	unsigned char							mode = 0;
	std::vector<std::pair<size_t, size_t>>	volumes_and_instances;
	template<class Archive> void serialize(Archive &ar) { ar(mode, volumes_and_instances); }
};

class StackImpl;

class Stack
{
public:
	// Stack needs to be initialized. An empty stack is not valid, there must be a "New Project" status stored at the beginning.
	// The first "New Project" snapshot shall not be removed.
	Stack();
	~Stack();

	void clear();
	bool empty() const;

	// Set maximum memory threshold. If the threshold is exceeded, least recently used snapshots are released.
	void set_memory_limit(size_t memsize);
	size_t get_memory_limit() const;

	// Estimate size of the RAM consumed by the Undo / Redo stack.
	size_t memsize() const;

	// Release least recently used snapshots up to the memory limit set above.
	void release_least_recently_used();

	// Store the current application state onto the Undo / Redo stack, remove all snapshots after m_active_snapshot_time.
    void take_snapshot(const std::string& snapshot_name, const Slic3r::Model& model, const Slic3r::GUI::Selection& selection, const Slic3r::GUI::GLGizmosManager& gizmos, const SnapshotData &snapshot_data);

    void take_snapshot(const std::string& snapshot_name, const Slic3r::Model& model, const Slic3r::GUI::Selection& selection, const Slic3r::GUI::GLGizmosManager& gizmos, const Slic3r::GUI::PartPlateList& plate_list, const SnapshotData& snapshot_data);

    // To be called just after take_snapshot() when leaving a gizmo, inside which small edits like support point add / remove events or paiting actions were allowed.
    // Remove all but the last edit between the gizmo enter / leave snapshots.
    void reduce_noisy_snapshots(const std::string& new_name);

	// To be queried to enable / disable the Undo / Redo buttons at the UI.
	bool has_undo_snapshot() const;
	bool has_redo_snapshot() const;
	// To query whether one can undo to a snapshot. Useful for notifications, that want to Undo a specific operation.
	bool has_undo_snapshot(size_t time_to_load) const;

	// Roll back the time. If time_to_load is SIZE_MAX, the previous snapshot is activated.
	// Undoing an action may need to take a snapshot of the current application state, so that redo to the current state is possible.
    bool undo(Slic3r::Model& model, const Slic3r::GUI::Selection& selection, Slic3r::GUI::GLGizmosManager& gizmos, Slic3r::GUI::PartPlateList& plate_list, const SnapshotData &snapshot_data, size_t time_to_load = SIZE_MAX);

	// Jump forward in time. If time_to_load is SIZE_MAX, the next snapshot is activated.
    bool redo(Slic3r::Model& model, Slic3r::GUI::GLGizmosManager& gizmos, Slic3r::GUI::PartPlateList& plate_list, size_t time_to_load = SIZE_MAX);

	// Snapshot history (names with timestamps).
	// Each snapshot indicates start of an interval in which this operation is performed.
	// There is one additional snapshot taken at the very end, which indicates the current unnamed state.

	const std::vector<Snapshot>& snapshots() const;
	const Snapshot& 		     snapshot(size_t time) const;

	// Timestamp of the active snapshot. One of the snapshots of this->snapshots() shall have Snapshot::timestamp equal to this->active_snapshot_time().
	// The active snapshot may be a special placeholder "@@@ Topmost @@@" indicating an uncaptured current state,
	// or the active snapshot may be an active state to which the application state was undoed or redoed.
	size_t active_snapshot_time() const;
	const Snapshot& active_snapshot() const { return this->snapshot(this->active_snapshot_time()); }
	// Temporary snapshot is active if the topmost snapshot is active and it has not been captured yet.
	// In that case the Undo action will capture the last snapshot.
	bool   temp_snapshot_active() const;

	// Resets the "dirty project" status.
    void   mark_current_as_saved();
    // Is the project modified with regard to the last "saved" state marked with mark_current_as_saved()?
    bool   project_modified() const;
	// BBS: backup and restore
	bool has_real_change_from(size_t time) const;

	// After load_snapshot() / undo() / redo() the selection is deserialized into a list of ObjectIDs, which needs to be converted
	// into the list of GLVolume pointers once the 3D scene is updated.
	const Selection& selection_deserialized() const;

private:
	friend class StackImpl;
	std::unique_ptr<StackImpl> 	pimpl;
};

}; // namespace UndoRedo
}; // namespace Slic3r

#endif /* slic3r_Utils_UndoRedo_hpp_ */
