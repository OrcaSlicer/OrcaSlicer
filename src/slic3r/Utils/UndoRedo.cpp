#include "UndoRedo.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <memory>
#include <typeinfo>
#include <cassert>
#include <cstddef>

#include <cereal/types/polymorphic.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/archives/binary.hpp>
#define CEREAL_FUTURE_EXPERIMENTAL
#include <cereal/archives/adapters.hpp>

#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/ObjectID.hpp>
#include <libslic3r/Utils.hpp>

#include "slic3r/GUI/3DScene.hpp"
#include <boost/foreach.hpp>

#ifndef NDEBUG
// #define SLIC3R_UNDOREDO_DEBUG
#endif /* NDEBUG */
#if 0
	// Stop at a fraction of the normal Undo / Redo stack size.
	#define UNDO_REDO_DEBUG_LOW_MEM_FACTOR 10000
#else
	#define UNDO_REDO_DEBUG_LOW_MEM_FACTOR 1
#endif

namespace Slic3r {
namespace UndoRedo {

#ifdef SLIC3R_UNDOREDO_DEBUG
static inline std::string ptr_to_string(const void* ptr)
{
    char buf[64];
    sprintf(buf, "%p", ptr);
    return buf;
}
#endif

static std::string topmost_snapshot_name = "@@@ Topmost @@@";

bool Snapshot::is_topmost() const
{
	return this->name == topmost_snapshot_name;
}

// Time interval, start is closed, end is open.
struct Interval
{
public:
	Interval(size_t begin, size_t end) : m_begin(begin), m_end(end) {}

	size_t  begin() const { return m_begin; }
	size_t  end()   const { return m_end; }

	bool 	is_valid() const { return m_begin >= 0 && m_begin < m_end; }
	// This interval comes strictly before the rhs interval.
	bool 	strictly_before(const Interval &rhs) const { return this->is_valid() && rhs.is_valid() && m_end <= rhs.m_begin; }
	// This interval comes strictly after the rhs interval.
	bool 	strictly_after(const Interval &rhs) const { return this->is_valid() && rhs.is_valid() && rhs.m_end <= m_begin; }

	bool    operator<(const Interval &rhs) const { return (m_begin < rhs.m_begin) || (m_begin == rhs.m_begin && m_end < rhs.m_end); }
	bool 	operator==(const Interval &rhs) const { return m_begin == rhs.m_begin && m_end == rhs.m_end; }

	void 	trim_begin(size_t new_begin)  { m_begin = std::max(m_begin, new_begin); }
	void    trim_end(size_t new_end) { m_end = std::min(m_end, new_end); }
	void 	extend_begin(size_t new_begin) { assert(new_begin <= m_begin); m_begin = new_begin; }
	void 	extend_end(size_t new_end) { assert(new_end >= m_end); m_end = new_end; }

	size_t 	memsize() const { return sizeof(this); }

private:
	size_t 	m_begin;
	size_t 	m_end;
};

// History of a single object tracked by the Undo / Redo stack. The object may be mutable or immutable.
class ObjectHistoryBase
{
public:
	virtual ~ObjectHistoryBase() {}

	// Is the object captured by this history mutable or immutable?
	virtual bool is_mutable() const = 0;
	virtual bool is_immutable() const = 0;
	// The object is optional, it may be released if the Undo / Redo stack memory grows over the limits.
	virtual bool is_optional() const { return false; }
	// If it is an immutable object, return its pointer. There is a map assigning a temporary ObjectID to the immutable object pointer.
	virtual const void* immutable_object_ptr() const { return nullptr; }

	// If the history is empty, the ObjectHistory object could be released.
	virtual bool empty() = 0;

	// Release all data before the given timestamp. For the ImmutableObjectHistory, the shared pointer is NOT released.
	// Return the amount of memory released.
	virtual size_t release_before_timestamp(size_t timestamp) = 0;
	// Release all data after the given timestamp. For the ImmutableObjectHistory, the shared pointer is NOT released.
	// Return the amount of memory released.
	virtual size_t release_after_timestamp(size_t timestamp) = 0;
	// Release all data between the two timestamps. For the ImmutableObjectHistory, the shared pointer is NOT released.
	// Used for reducing the number of snapshots for noisy operations like the support point edits.
	// Return the amount of memory released.
	virtual size_t release_between_timestamps(size_t timestamp_start, size_t timestamp_end) = 0;
	// Release all optional data of this history.
	virtual size_t release_optional() = 0;
	// Restore optional data possibly released by release_optional.
	virtual void   restore_optional() = 0;

	// Estimated size in memory, to be used to drop least recently used snapshots.
	virtual size_t memsize() const = 0;

#ifdef SLIC3R_UNDOREDO_DEBUG
	// Human readable debug information.
	virtual std::string	format() = 0;
#endif /* SLIC3R_UNDOREDO_DEBUG */

#ifndef NDEBUG
	virtual bool valid() = 0;
#endif /* NDEBUG */
};

template<typename T> class ObjectHistory : public ObjectHistoryBase
{
public:
	~ObjectHistory() override {}

	// If the history is empty, the ObjectHistory object could be released.
	bool empty() override { return m_history.empty(); }

	// Release all data before the given timestamp. For the ImmutableObjectHistory, the shared pointer is NOT released.
	size_t release_before_timestamp(size_t timestamp) override {
		size_t mem_released = 0;
		if (! m_history.empty()) {
			assert(this->valid());
			// it points to an interval which either starts with timestamp, or follows the timestamp.
			auto it = std::lower_bound(m_history.begin(), m_history.end(), T(timestamp, timestamp));
			// Find the first iterator with begin() < timestamp.
			if (it == m_history.end())
				-- it;
			while (it != m_history.begin() && it->begin() >= timestamp)
				-- it;
			if (it->begin() < timestamp && it->end() > timestamp) {
				it->trim_begin(timestamp);
				if (it != m_history.begin())
					-- it;
			}
			if (it->end() <= timestamp) {
				auto it_end = ++ it;
				for (it = m_history.begin(); it != it_end; ++ it)
					mem_released += it->memsize();
				m_history.erase(m_history.begin(), it_end);
			}
			assert(this->valid());
		}
		return mem_released;
	}

	// Release all data after the given timestamp. The shared pointer is NOT released.
	size_t release_after_timestamp(size_t timestamp) override {
		size_t mem_released = 0;
		if (! m_history.empty()) {
			assert(this->valid());
			// it points to an interval which either starts with timestamp, or follows the timestamp.
			auto it = std::lower_bound(m_history.begin(), m_history.end(), T(timestamp, timestamp));
			if (it != m_history.begin()) {
				auto it_prev = it;
				-- it_prev;
				assert(it_prev->begin() < timestamp);
				// Trim the last interval with timestamp.
				it_prev->trim_end(timestamp);
			}
			for (auto it2 = it; it2 != m_history.end(); ++ it2)
				mem_released += it2->memsize();
			m_history.erase(it, m_history.end());
			assert(this->valid());
		}
		return mem_released;
	}

	// Release all data between the two timestamps. For the ImmutableObjectHistory, the shared pointer is NOT released.
	// Used for reducing the number of snapshots for noisy operations like the support point edits.
	// Return the amount of memory released.
	size_t release_between_timestamps(size_t timestamp_start, size_t timestamp_end) override {
		size_t mem_released = 0;
		if (! m_history.empty()) {
			assert(this->valid());
			// Find the span of m_history intervals that are fully in (timestamp_start, timestamp_end>, thus they will be never
			// deserialized for any snapshot in <timestamp_start, timestamp_end).
			auto it_lo = std::upper_bound(m_history.begin(), m_history.end(), timestamp_start, [](size_t l, const auto &r) { return l < r.begin(); });
			auto it_hi = std::upper_bound(it_lo, m_history.end(), timestamp_end, [](size_t l, const auto &r) { return l < r.end(); });
			if (it_lo != it_hi) {
				// There are some intervals that start inside (timestamp_start, timestamp_end), that could be released.
				assert(it_lo->begin() > timestamp_start && it_lo->end() <= timestamp_end);
				assert(it_hi == m_history.end() || (it_hi->begin() > it_lo->begin() && it_hi->end() > timestamp_end));
				if (it_lo != m_history.begin() && it_hi != m_history.end()) {
					// One may consider merging the two intervals.
					//FIXME merge them.
				}
				for (auto it = it_lo; it != it_hi; ++ it)
					mem_released += it->memsize();
				m_history.erase(it_lo, it_hi);
			}
			assert(this->valid());
		}
		return mem_released;
	}

protected:
	std::vector<T>	m_history;
};

// Big objects (mainly the triangle meshes) are tracked by Slicer using the shared pointers
// and they are immutable.
// The Undo / Redo stack therefore may keep a shared pointer to these immutable objects
// and as long as the ref counter of these objects is higher than 1 (1 reference is held
// by the Undo / Redo stack), there is no cost associated to holding the object
// at the Undo / Redo stack. Once the reference counter drops to 1 (only the Undo / Redo
// stack holds the reference), the shared pointer may get serialized (and possibly compressed)
// and the shared pointer may be released.
// The history of a single immutable object may not be continuous, as an immutable object may
// be removed from the scene while being kept at the Copy / Paste stack.
template<typename T>
class ImmutableObjectHistory : public ObjectHistory<Interval>
{
public:
	ImmutableObjectHistory(std::shared_ptr<const T>	shared_object, bool optional) : m_shared_object(shared_object), m_optional(optional) {}
	~ImmutableObjectHistory() override {}

	bool is_mutable() const override { return false; }
	bool is_immutable() const override { return true; }
	bool is_optional() const override { return m_optional; }
	// If it is an immutable object, return its pointer. There is a map assigning a temporary ObjectID to the immutable object pointer.
    const void* immutable_object_ptr() const override { return (const void*)m_shared_object.get(); }

	// Estimated size in memory, to be used to drop least recently used snapshots.
	size_t memsize() const override {
		size_t memsize = sizeof(*this);
		if (this->is_serialized())
			memsize += m_serialized.size();
		else if (m_shared_object.use_count() == 1)
			// Only count the shared object's memsize into the total Undo / Redo stack memsize if it is referenced from the Undo / Redo stack only.
			memsize += m_shared_object->memsize();
		memsize += m_history.size() * sizeof(Interval);
		return memsize;
	}

	void save(size_t active_snapshot_time, size_t current_time) {
		assert(m_history.empty() || m_history.back().end() <= active_snapshot_time ||
			// The snapshot of an immutable object may have already been taken from another mutable object.
			(m_history.back().begin() <= active_snapshot_time && m_history.back().end() == current_time + 1));
		if (m_history.empty() || m_history.back().end() < active_snapshot_time)
			m_history.emplace_back(active_snapshot_time, current_time + 1);
		else
			m_history.back().extend_end(current_time + 1);
	}

	bool has_snapshot(size_t timestamp) {
		if (m_history.empty())
			return false;
		auto it = std::lower_bound(m_history.begin(), m_history.end(), Interval(timestamp, timestamp));
		if (it == m_history.end() || it->begin() > timestamp) {
			if (it == m_history.begin())
				return false;
			-- it;
		}
		return timestamp >= it->begin() && timestamp < it->end();
	}

	// Release all optional data of this history.
	size_t release_optional() override {
		size_t mem_released = 0;
		if (m_optional) {
			bool released = false;
			if (this->is_serialized()) {
				mem_released += m_serialized.size();
				m_serialized.clear();
				released = true;
			} else if (m_shared_object.use_count() == 1) {
				mem_released += m_shared_object->memsize();
				m_shared_object.reset();
				released = true;
			}
			if (released) {
				mem_released += m_history.size() * sizeof(Interval);
				m_history.clear();
			}
		} else if (m_shared_object.use_count() == 1) {
			// The object is in memory, but it is not shared with the scene. Let the object decide whether there is any optional data to release.
			const_cast<T*>(m_shared_object.get())->release_optional();
		}
		return mem_released;
	}

	// Restore optional data possibly released by this->release_optional().
	void restore_optional() override {
		if (m_shared_object.use_count() == 1)
			const_cast<T*>(m_shared_object.get())->restore_optional();
	}

	bool 						is_serialized() const { return m_shared_object.get() == nullptr; }
	const std::string&			serialized_data() const { return m_serialized; }
	std::shared_ptr<const T>& 	shared_ptr(StackImpl &stack);

#ifdef SLIC3R_UNDOREDO_DEBUG
	std::string 				format() override {
		std::string out = typeid(T).name();
		out += this->is_serialized() ?
			std::string(" len:") + std::to_string(m_serialized.size()) :
			std::string(" shared_ptr:") + ptr_to_string(m_shared_object.get());
		for (const Interval &interval : m_history)
			out += std::string(", <") + std::to_string(interval.begin()) + "," + std::to_string(interval.end()) + ")";
		return out;
	}
#endif /* SLIC3R_UNDOREDO_DEBUG */

#ifndef NDEBUG
	bool 						valid() override;
#endif /* NDEBUG */

private:
	// Either the source object is held by a shared pointer and the m_serialized field is empty,
	// or the shared pointer is null and the object is being serialized into m_serialized.
	std::shared_ptr<const T>	m_shared_object;
	// If this object is optional, then it may be deleted from the Undo / Redo stack and recalculated from other data (for example mesh convex hull).
	bool 						m_optional;
	std::string 				m_serialized;
};

struct MutableHistoryInterval
{
private:
	struct Data
	{
		// Reference counter of this data chunk. We may have used shared_ptr, but the shared_ptr is thread safe
		// with the associated cost of CPU cache invalidation on refcount change.
		size_t		refcnt;
		size_t		size;
		char 		data[1];

		// The serialized data matches the data stored here.
		bool 		matches(const std::string& rhs) { return this->size == rhs.size() && memcmp(this->data, rhs.data(), this->size) == 0; }

		// The timestamp matches the timestamp serialized in the data stored here.
		bool 		matches_timestamp(uint64_t timestamp) { assert(timestamp > 0);  assert(this->size > 8); return memcmp(this->data, &timestamp, 8) == 0; }
	};

	Interval    m_interval;
	Data	   *m_data;

public:
	MutableHistoryInterval(const Interval &interval, const std::string &input_data) : m_interval(interval), m_data(nullptr) {
		m_data = (Data*)new char[offsetof(Data, data) + input_data.size()];
		m_data->refcnt = 1;
		m_data->size = input_data.size();
		memcpy(m_data->data, input_data.data(), input_data.size());
	}

	MutableHistoryInterval(const Interval &interval, MutableHistoryInterval &other) : m_interval(interval), m_data(other.m_data) {
		++ m_data->refcnt;
	}

	// as a key for std::lower_bound
	MutableHistoryInterval(const size_t begin, const size_t end) : m_interval(begin, end), m_data(nullptr) {}

	MutableHistoryInterval(MutableHistoryInterval&& rhs) : m_interval(rhs.m_interval), m_data(rhs.m_data) { rhs.m_data = nullptr; }
	MutableHistoryInterval& operator=(MutableHistoryInterval&& rhs) { m_interval = rhs.m_interval; m_data = rhs.m_data; rhs.m_data = nullptr; return *this; }

	~MutableHistoryInterval() {
		if (m_data != nullptr && -- m_data->refcnt == 0)
			delete[] (char*)m_data;
	}

	const Interval& interval() const { return m_interval; }
	size_t		begin() const { return m_interval.begin(); }
	size_t		end()   const { return m_interval.end(); }
	void 		trim_begin  (size_t timestamp) { m_interval.trim_begin(timestamp); }
	void 		trim_end    (size_t timestamp) { m_interval.trim_end(timestamp); }
	void 		extend_begin(size_t timestamp) { m_interval.extend_begin(timestamp); }
	void 		extend_end  (size_t timestamp) { m_interval.extend_end(timestamp); }

	bool		operator<(const MutableHistoryInterval& rhs) const { return m_interval < rhs.m_interval; }
	bool 		operator==(const MutableHistoryInterval& rhs) const { return m_interval == rhs.m_interval; }

	const char* data() const { return m_data->data; }
	size_t  	size() const { return m_data->size; }
	size_t		refcnt() const { return m_data->refcnt; }
	bool		matches(const std::string& data) { return m_data->matches(data); }
	bool		matches_timestamp(uint64_t timestamp) { return m_data->matches_timestamp(timestamp); }
	size_t 		memsize() const {
		return m_data->refcnt == 1 ?
			// Count just the size of the snapshot data.
			m_data->size :
			// Count the size of the snapshot data divided by the number of references, rounded up.
			(m_data->size + m_data->refcnt - 1) / m_data->refcnt;
	}

private:
	MutableHistoryInterval(const MutableHistoryInterval &rhs);
	MutableHistoryInterval& operator=(const MutableHistoryInterval &rhs);
};

// Smaller objects (Model, ModelObject, ModelInstance, ModelVolume, DynamicPrintConfig)
// are mutable and there is not tracking of the changes, therefore a snapshot needs to be
// taken every time and compared to the previous data at the Undo / Redo stack.
// The serialized data is stored if it is different from the last value on the stack, otherwise
// the serialized data is discarded.
// The history of a single mutable object may not be continuous, as an mutable object may
// be removed from the scene while being kept at the Copy / Paste stack, therefore an object snapshot
// with the same serialized object data may be shared by multiple history intervals.
template<typename T>
class MutableObjectHistory : public ObjectHistory<MutableHistoryInterval>
{
public:
	~MutableObjectHistory() override {}

	bool is_mutable() const override { return true; }
	bool is_immutable() const override { return false; }

	// Estimated size in memory, to be used to drop least recently used snapshots.
	size_t memsize() const override {
		size_t memsize = sizeof(*this);
		memsize += m_history.size() * sizeof(MutableHistoryInterval);
		for (const MutableHistoryInterval &interval : m_history)
			memsize += interval.memsize();
		return memsize;
	}

	// If an object provides a reliable timestamp and the object serializes the timestamp first,
	// then we may just check the validity of the timestamp against the last snapshot without
	// having to serialize the whole object. This reduces the amount of serialization and memcmp
	// when taking a snapshot.
	bool try_save_timestamp(size_t active_snapshot_time, size_t current_time, uint64_t timestamp) {
		assert(m_history.empty() || m_history.back().end() <= active_snapshot_time);
		if (! m_history.empty() && m_history.back().matches_timestamp(timestamp)) {
			if (m_history.back().end() < active_snapshot_time)
				// Share the previous data by reference counting.
				m_history.emplace_back(Interval(current_time, current_time + 1), m_history.back());
			else {
				assert(m_history.back().end() == active_snapshot_time);
				// Just extend the last interval using the old data.
				m_history.back().extend_end(current_time + 1);
			}
			return true;
		}
		// The timestamp is not valid, the caller has to call this->save() with the serialized data.
		return false;
	}

	void save(size_t active_snapshot_time, size_t current_time, const std::string &data) {
		assert(m_history.empty() || m_history.back().end() <= active_snapshot_time);
		if (m_history.empty() || m_history.back().end() < active_snapshot_time) {
			if (! m_history.empty() && m_history.back().matches(data))
				// Share the previous data by reference counting.
				m_history.emplace_back(Interval(current_time, current_time + 1), m_history.back());
			else
				// Allocate new data.
				m_history.emplace_back(Interval(current_time, current_time + 1), data);
		} else {
			assert(! m_history.empty());
			assert(m_history.back().end() == active_snapshot_time);
			if (m_history.back().matches(data))
				// Just extend the last interval using the old data.
				m_history.back().extend_end(current_time + 1);
			else
				// Allocate new data time continuous with the previous data.
				m_history.emplace_back(Interval(active_snapshot_time, current_time + 1), data);
		}
	}

	std::string load(size_t timestamp) const {
		assert(! m_history.empty());
		auto it = std::lower_bound(m_history.begin(), m_history.end(), MutableHistoryInterval(timestamp, timestamp));
		if (it == m_history.end() || it->begin() > timestamp) {
			//assert(it != m_history.begin());
			if (it != m_history.begin())
				--it;
		}
		//assert(timestamp >= it->begin() && timestamp < it->end());
		return std::string(it->data(), it->data() + it->size());
	}

	// Currently all mutable snapshots are mandatory.
	size_t release_optional() override { return 0; }
	// Currently there is no way to release optional data from the mutable objects.
	void   restore_optional() override {}

#ifdef SLIC3R_UNDOREDO_DEBUG
	std::string format() override {
		std::string out = typeid(T).name();
		for (const MutableHistoryInterval &interval : m_history)
			out += std::string(", ptr:") + ptr_to_string(interval.data()) + " len:" + std::to_string(interval.size()) + " <" + std::to_string(interval.begin()) + "," + std::to_string(interval.end()) + ")";
		return out;
	}
#endif /* SLIC3R_UNDOREDO_DEBUG */

#ifndef NDEBUG
	bool valid() override;
#endif /* NDEBUG */
};

#ifndef NDEBUG
template<typename T>
bool ImmutableObjectHistory<T>::valid()
{
	// The immutable object content is captured either by a shared object, or by its serialization, but not both.
	assert(! m_shared_object == ! m_serialized.empty());
	// Verify that the history intervals are sorted and do not overlap.
	if (! m_history.empty())
		for (size_t i = 1; i < m_history.size(); ++ i)
			assert(m_history[i - 1].strictly_before(m_history[i]));
	return true;
}
#endif /* NDEBUG */

#ifndef NDEBUG
template<typename T>
bool MutableObjectHistory<T>::valid()
{
	// Verify that the history intervals are sorted and do not overlap, and that the data reference counters are correct.
	if (! m_history.empty()) {
		std::map<const char*, size_t> refcntrs;
		assert(m_history.front().data() != nullptr);
		++ refcntrs[m_history.front().data()];
		for (size_t i = 1; i < m_history.size(); ++ i) {
			assert(m_history[i - 1].interval().strictly_before(m_history[i].interval()));
			++ refcntrs[m_history[i].data()];
		}
		for (const auto &hi : m_history) {
			assert(hi.data() != nullptr);
			assert(refcntrs[hi.data()] == hi.refcnt());
		}
	}
	return true;
}
#endif /* NDEBUG */

class StackImpl
{
public:
	// Stack needs to be initialized. An empty stack is not valid, there must be a "New Project" status stored at the beginning.
	// Initially enable Undo / Redo stack to occupy maximum 10% of the total system physical memory.
	StackImpl() : m_memory_limit(std::min(Slic3r::total_physical_memory() / 10, size_t(1 * 16384 * 65536 / UNDO_REDO_DEBUG_LOW_MEM_FACTOR))), m_active_snapshot_time(0), m_current_time(0) {}

	void clear() {
		m_objects.clear();
		m_shared_ptr_to_object_id.clear();
		m_snapshots.clear();
		m_active_snapshot_time = 0;
		m_current_time = 0;
		m_saved_snapshot_time = size_t(-1);
		m_selection.clear();
	}

	bool empty() const {
		assert(m_objects.empty() == m_snapshots.empty());
		assert(! m_objects.empty() || (m_current_time == 0 && m_active_snapshot_time == 0));
		return m_snapshots.empty();
	}

	void set_memory_limit(size_t memsize) { m_memory_limit = memsize; }
	size_t get_memory_limit() const { return m_memory_limit; }

	size_t memsize() const {
		size_t memsize = 0;
		for (const auto &object : m_objects)
			memsize += object.second->memsize();
		return memsize;
	}

    // Store the current application state onto the Undo / Redo stack, remove all snapshots after m_active_snapshot_time.
	void take_snapshot(const std::string& snapshot_name, const Slic3r::Model& model, const Slic3r::GUI::Selection& selection, const Slic3r::GUI::GLGizmosManager& gizmos, const Slic3r::GUI::PartPlateList& plate_list, const SnapshotData& snapshot_data);
    void take_snapshot(const std::string& snapshot_name, const Slic3r::Model& model, const Slic3r::GUI::Selection& selection, const Slic3r::GUI::GLGizmosManager& gizmos, const SnapshotData &snapshot_data);
    void reduce_noisy_snapshots(const std::string& new_name);
    void load_snapshot(size_t timestamp, Slic3r::Model& model, Slic3r::GUI::GLGizmosManager& gizmos, Slic3r::GUI::PartPlateList& plate_list);

	bool has_undo_snapshot() const;
	bool has_undo_snapshot(size_t time_to_load) const;
	bool has_redo_snapshot() const;
    bool undo(Slic3r::Model &model, const Slic3r::GUI::Selection &selection, Slic3r::GUI::GLGizmosManager &gizmos, Slic3r::GUI::PartPlateList& plate_list, const SnapshotData &snapshot_data, size_t jump_to_time);
    bool redo(Slic3r::Model &model, Slic3r::GUI::GLGizmosManager &gizmos, Slic3r::GUI::PartPlateList& plate_list, size_t jump_to_time);
	void release_least_recently_used();

	// Snapshot history (names with timestamps).
	const std::vector<Snapshot>& 	snapshots() const { return m_snapshots; }
	const Snapshot&				    snapshot(size_t time) const {
		const auto it = std::lower_bound(m_snapshots.cbegin(), m_snapshots.cend(), UndoRedo::Snapshot(time));
		assert(it != m_snapshots.end() && it->timestamp == time);
		return *it;
	}

	// Timestamp of the active snapshot.
	size_t 							active_snapshot_time() const { return m_active_snapshot_time; }
	bool 							temp_snapshot_active() const { return m_snapshots.back().timestamp == m_active_snapshot_time && ! m_snapshots.back().is_topmost_captured(); }

	// Resets the "dirty project" status.
    void 							mark_current_as_saved() { m_saved_snapshot_time = m_active_snapshot_time; }
    bool 							project_modified() const;

	// BBS: backup and restore
    bool has_real_change_from(size_t time) const;

    const Selection &selection_deserialized() const { return m_selection; }

//protected:
	template<typename T> ObjectID save_mutable_object(const T &object);
	template<typename T> ObjectID save_immutable_object(std::shared_ptr<const T> &object, bool optional);
	template<typename T> T* load_mutable_object(const Slic3r::ObjectID id);
	template<typename T> std::shared_ptr<const T> load_immutable_object(const Slic3r::ObjectID id, bool optional);
	template<typename T> void load_mutable_object(const Slic3r::ObjectID id, T &target);

#ifdef SLIC3R_UNDOREDO_DEBUG
	std::string format() const {
		std::string out = "Objects\n";
		for (const std::pair<const ObjectID, std::unique_ptr<ObjectHistoryBase>> &kvp : m_objects)
			out += std::string("ObjectID:") + std::to_string(kvp.first.id) + " " + kvp.second->format() + "\n";
		out += "Snapshots\n";
		for (const Snapshot &snapshot : m_snapshots) {
			if (snapshot.timestamp == m_active_snapshot_time)
				out += ">>> ";
			out += std::string("Name: \"") + snapshot.name + "\", timestamp: " + std::to_string(snapshot.timestamp) +
				", Model ID:" + ((snapshot.model_id == 0) ? "Invalid" : std::to_string(snapshot.model_id)) + "\n";
		}
		if (m_active_snapshot_time > m_snapshots.back().timestamp)
			out += ">>>\n";
		out += "Current time: " + std::to_string(m_current_time) + "\n";
		out += "Total memory occupied: " + std::to_string(this->memsize()) + "\n";
		return out;
	}
	void print() const {
		std::cout << "Undo / Redo stack" << std::endl;
		std::cout << this->format() << std::endl;
	}
#endif /* SLIC3R_UNDOREDO_DEBUG */


#ifndef NDEBUG
	bool valid() const {
		//assert(! m_snapshots.empty());
		if (m_snapshots.empty())
			return false;
		assert(m_snapshots.back().is_topmost());
		auto it = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), Snapshot(m_active_snapshot_time));
		assert(it != m_snapshots.begin() && it != m_snapshots.end() && it->timestamp == m_active_snapshot_time);
		assert(m_active_snapshot_time <= m_snapshots.back().timestamp);
		for (auto it = m_objects.begin(); it != m_objects.end(); ++ it)
			assert(it->second->valid());
		return true;
	}
#endif /* NDEBUG */

private:
	template<typename T> ObjectID 	immutable_object_id(const std::shared_ptr<const T> &ptr) {
		return this->immutable_object_id_impl((const void*)ptr.get());
	}
	ObjectID     					immutable_object_id_impl(const void *ptr) {
		auto it = m_shared_ptr_to_object_id.find(ptr);
		if (it == m_shared_ptr_to_object_id.end()) {
			// Allocate a new temporary ObjectID for this shared pointer.
			ObjectBase object_with_id;
			it = m_shared_ptr_to_object_id.insert(it, std::make_pair(ptr, object_with_id.id()));
		}
		return it->second;
	}
	void 							collect_garbage();

	// Release snapshots between begin and end. Only erases data from m_snapshots, not from m_objects!
	// Updates m_saved_snapshot_time.
	std::vector<Snapshot>::iterator release_snapshots(std::vector<Snapshot>::iterator begin, std::vector<Snapshot>::iterator end);

	// Maximum memory allowed to be occupied by the Undo / Redo stack. If the limit is exceeded,
	// least recently used snapshots will be released.
	size_t 													m_memory_limit;
	// Each individual object (Model, ModelObject, ModelInstance, ModelVolume, Selection, TriangleMesh)
	// is stored with its own history, referenced by the ObjectID. Immutable objects do not provide
	// their own IDs, therefore there are temporary IDs generated for them and stored to m_shared_ptr_to_object_id.
	std::map<ObjectID, std::unique_ptr<ObjectHistoryBase>> 	m_objects;
	std::map<const void*, ObjectID>							m_shared_ptr_to_object_id;
	// Snapshot history (names with timestamps).
	std::vector<Snapshot>									m_snapshots;
	// Timestamp of the active snapshot.
	size_t 													m_active_snapshot_time;
	// Time at which the project state was saved into a project file.
	// If set to zero, the time is not known, thus the project state should be considered unsaved.
	size_t 													m_saved_snapshot_time { size_t(-1) };
	// Logical time counter. m_current_time is being incremented with each snapshot taken.
	size_t 													m_current_time;
	// Last selection serialized or deserialized.
	Selection 												m_selection;
	std::vector<ObjectBase*> 								m_reusable_objects;
};

using InputArchive  = cereal::UserDataAdapter<StackImpl, cereal::BinaryInputArchive>;
using OutputArchive = cereal::UserDataAdapter<StackImpl, cereal::BinaryOutputArchive>;

} // namespace UndoRedo

class Model;
class ModelObject;
class ModelVolume;
class ModelInstance;
class ModelMaterial;
class DynamicPrintConfig;
class TriangleMesh;

} // namespace Slic3r

namespace cereal
{
	// Let cereal know that there are load / save non-member functions declared for ModelObject*, ignore serialization of pointers triggering
	// static assert, that cereal does not support serialization of raw pointers.
	template <class Archive> struct specialize<Archive, Slic3r::Model*, cereal::specialization::non_member_load_save> {};
	template <class Archive> struct specialize<Archive, Slic3r::ModelObject*, cereal::specialization::non_member_load_save> {};
	template <class Archive> struct specialize<Archive, Slic3r::ModelVolume*, cereal::specialization::non_member_load_save> {};
	template <class Archive> struct specialize<Archive, Slic3r::ModelInstance*, cereal::specialization::non_member_load_save> {};
	template <class Archive> struct specialize<Archive, Slic3r::ModelMaterial*, cereal::specialization::non_member_load_save> {};
	template <class Archive> struct specialize<Archive, std::shared_ptr<Slic3r::TriangleMesh>, cereal::specialization::non_member_load_save> {};
	template <class Archive> struct specialize<Archive, Slic3r::GUI::PartPlate*, cereal::specialization::non_member_load_save> {};

	// Store ObjectBase derived class onto the Undo / Redo stack as a separate object,
	// store just the ObjectID to this stream.
	template <class T> void save(BinaryOutputArchive& ar, T* const& ptr)
	{
		ar(cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar).save_mutable_object<T>(*ptr));
	}

	// Load ObjectBase derived class from the Undo / Redo stack as a separate object
	// based on the ObjectID loaded from this stream.
	template <class T> void load(BinaryInputArchive& ar, T*& ptr)
	{
		Slic3r::UndoRedo::StackImpl& stack = cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar);
		size_t id;
		ar(id);
		ptr = stack.load_mutable_object<T>(Slic3r::ObjectID(id));
	}

	// Store ObjectBase derived class onto the Undo / Redo stack as a separate object,
	// store just the ObjectID to this stream.
	template <class T> void save(BinaryOutputArchive &ar, const std::unique_ptr<T> &ptr)
	{
		ar(cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar).save_mutable_object<T>(*ptr.get()));
	}

	// Load ObjectBase derived class from the Undo / Redo stack as a separate object
	// based on the ObjectID loaded from this stream.
	template <class T> void load(BinaryInputArchive &ar, std::unique_ptr<T> &ptr)
	{
		Slic3r::UndoRedo::StackImpl& stack = cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar);
		size_t id;
		ar(id);
		ptr.reset(stack.load_mutable_object<T>(Slic3r::ObjectID(id)));
	}

	// Store ObjectBase derived class onto the Undo / Redo stack as a separate object,
	// store just the ObjectID to this stream.
	template<class T> void save_by_value(BinaryOutputArchive& ar, const T &cfg)
	{
		ar(cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar).save_mutable_object<T>(cfg));
	}
	// Load ObjectBase derived class from the Undo / Redo stack as a separate object
	// based on the ObjectID loaded from this stream.
	template<class T> void load_by_value(BinaryInputArchive& ar, T &cfg)
	{
		Slic3r::UndoRedo::StackImpl& stack = cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar);
		size_t id;
		ar(id);
		stack.load_mutable_object<T>(Slic3r::ObjectID(id), cfg);
	}

	// Store ObjectBase derived class onto the Undo / Redo stack as a separate object,
	// store just the ObjectID to this stream.
	template <class T> void save(BinaryOutputArchive &ar, const std::shared_ptr<const T> &ptr)
	{
		ar(cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar).save_immutable_object<T>(const_cast<std::shared_ptr<const T>&>(ptr), false));
	}
	template <class T> void save_optional(BinaryOutputArchive &ar, const std::shared_ptr<const T> &ptr)
	{
		ar(cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar).save_immutable_object<T>(const_cast<std::shared_ptr<const T>&>(ptr), true));
	}

	// Load ObjectBase derived class from the Undo / Redo stack as a separate object
	// based on the ObjectID loaded from this stream.
	template <class T> void load(BinaryInputArchive &ar, std::shared_ptr<const T> &ptr)
	{
		Slic3r::UndoRedo::StackImpl &stack = cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar);
		size_t id;
		ar(id);
		ptr = stack.load_immutable_object<T>(Slic3r::ObjectID(id), false);
	}
	template <class T> void load_optional(BinaryInputArchive &ar, std::shared_ptr<const T> &ptr)
	{
		Slic3r::UndoRedo::StackImpl &stack = cereal::get_user_data<Slic3r::UndoRedo::StackImpl>(ar);
		size_t id;
		ar(id);
		ptr = stack.load_immutable_object<T>(Slic3r::ObjectID(id), true);
	}
}

#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <slic3r/GUI/Selection.hpp>
#include <slic3r/GUI/Gizmos/GLGizmosManager.hpp>
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/PartPlate.hpp"

namespace Slic3r {
namespace UndoRedo {

template<typename T> std::shared_ptr<const T>& 	ImmutableObjectHistory<T>::shared_ptr(StackImpl &stack)
{
	if (m_shared_object.get() == nullptr && ! m_serialized.empty()) {
		// Deserialize the object.
		std::istringstream iss(m_serialized);
		{
			Slic3r::UndoRedo::InputArchive archive(stack, iss);
			typedef typename std::remove_const<T>::type Type;
			std::unique_ptr<Type> mesh(new Type());
			archive(*mesh.get());
			m_shared_object = std::move(mesh);
		}
	}
	return m_shared_object;
}

template<typename T> ObjectID StackImpl::save_mutable_object(const T &object)
{
	// First find or allocate a history stack for the ObjectID of this object instance.
	auto it_object_history = m_objects.find(object.id());
	if (it_object_history == m_objects.end())
		it_object_history = m_objects.insert(it_object_history, std::make_pair(object.id(), std::unique_ptr<MutableObjectHistory<T>>(new MutableObjectHistory<T>())));
	auto *object_history = static_cast<MutableObjectHistory<T>*>(it_object_history->second.get());
	bool  needs_to_save  = true;
	{
		// If the timestamp returned is non zero, then it is considered reliable.
		// The caller is supposed to serialize the timestamp first.
		uint64_t timestamp = object.timestamp();
		if (timestamp > 0)
			needs_to_save = ! object_history->try_save_timestamp(m_active_snapshot_time, m_current_time, timestamp);
	}
	if (needs_to_save) {
		// Serialize the object into a string.
		std::ostringstream oss;
		{
			Slic3r::UndoRedo::OutputArchive archive(*this, oss);
			archive(object);
		}
		object_history->save(m_active_snapshot_time, m_current_time, oss.str());
	}
	return object.id();
}

template<typename T> ObjectID StackImpl::save_immutable_object(std::shared_ptr<const T> &object, bool optional)
{
	// First allocate a temporary ObjectID for this pointer.
	ObjectID object_id = this->immutable_object_id(object);
	// and find or allocate a history stack for the ObjectID associated to this shared_ptr.
	auto it_object_history = m_objects.find(object_id);
	if (it_object_history == m_objects.end())
		it_object_history = m_objects.emplace_hint(it_object_history, object_id, std::unique_ptr<ImmutableObjectHistory<T>>(new ImmutableObjectHistory<T>(object, optional)));
	else
		assert(it_object_history->second.get()->is_optional() == optional);
	// Then save the interval.
	static_cast<ImmutableObjectHistory<T>*>(it_object_history->second.get())->save(m_active_snapshot_time, m_current_time);
	return object_id;
}

template<typename T> T* StackImpl::load_mutable_object(const Slic3r::ObjectID id)
{
	// BBS: reuse objects for backup
	auto it = std::find_if(m_reusable_objects.begin(), m_reusable_objects.end(), [id](auto o) { return o && o->id() == id; });
	T* target = nullptr;
	if (it == m_reusable_objects.end())
		target = new T();
	else {
		target = static_cast<T*>(*it);
		*it = nullptr;
	}
	this->load_mutable_object<T>(id, *target);
	return target;
}

template<typename T> std::shared_ptr<const T> StackImpl::load_immutable_object(const Slic3r::ObjectID id, bool optional)
{
	// First find a history stack for the ObjectID of this object instance.
	auto it_object_history = m_objects.find(id);
	assert(optional || it_object_history != m_objects.end());
	if (it_object_history == m_objects.end())
		return std::shared_ptr<const T>();
	auto *object_history = static_cast<ImmutableObjectHistory<T>*>(it_object_history->second.get());
	assert(object_history->has_snapshot(m_active_snapshot_time));
	object_history->restore_optional();
	return object_history->shared_ptr(*this);
}

template<typename T> void StackImpl::load_mutable_object(const Slic3r::ObjectID id, T &target)
{
	// First find a history stack for the ObjectID of this object instance.
	auto it_object_history = m_objects.find(id);
	assert(it_object_history != m_objects.end());
	auto *object_history = static_cast<const MutableObjectHistory<T>*>(it_object_history->second.get());
	// Then get the data associated with the object history and m_active_snapshot_time.
	std::istringstream iss(object_history->load(m_active_snapshot_time));
	Slic3r::UndoRedo::InputArchive archive(*this, iss);
	target.m_id = id;
	archive(target);
}

// Store the current application state onto the Undo / Redo stack, remove all snapshots after m_active_snapshot_time.
void StackImpl::take_snapshot(const std::string& snapshot_name, const Slic3r::Model& model, const Slic3r::GUI::Selection& selection, const Slic3r::GUI::GLGizmosManager& gizmos, const SnapshotData &snapshot_data)
{
	Slic3r::GUI::PartPlateList& plate_list = GUI::wxGetApp().plater()->get_partplate_list();

	take_snapshot(snapshot_name, model, selection, gizmos, plate_list, snapshot_data);

	return;
}

// Store the current application state onto the Undo / Redo stack, remove all snapshots after m_active_snapshot_time.
void StackImpl::take_snapshot(const std::string& snapshot_name, const Slic3r::Model& model, const Slic3r::GUI::Selection& selection, const Slic3r::GUI::GLGizmosManager& gizmos, const Slic3r::GUI::PartPlateList& plate_list, const SnapshotData& snapshot_data)
{
	// Release old snapshot data.
	assert(m_active_snapshot_time <= m_current_time);
	for (auto &kvp : m_objects)
		kvp.second->release_after_timestamp(m_active_snapshot_time);
	bool topmost_saved = false;
	if (! m_snapshots.empty()) {
		// If the project was saved for the topmost snapshot, restore the "saved" state after the "topmost" snapshot is taken.
		topmost_saved = m_active_snapshot_time == m_saved_snapshot_time && m_active_snapshot_time == m_snapshots.back().timestamp;
		auto it = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), Snapshot(m_active_snapshot_time));
		this->release_snapshots(it, m_snapshots.end());
	}
	// Take new snapshots.
	this->save_mutable_object<Slic3r::Model>(model);
	m_selection.volumes_and_instances.clear();
	m_selection.volumes_and_instances.reserve(selection.get_volume_idxs().size());
	m_selection.mode = selection.get_mode();
	for (unsigned int volume_idx : selection.get_volume_idxs())
		m_selection.volumes_and_instances.emplace_back(selection.get_volume(volume_idx)->geometry_id);
	this->save_mutable_object<Selection>(m_selection);
    this->save_mutable_object<Slic3r::GUI::GLGizmosManager>(gizmos);

	//BBS:save the partplater related data
	this->save_mutable_object<Slic3r::GUI::PartPlateList>(plate_list);

    // Save the snapshot info.
	m_snapshots.emplace_back(snapshot_name, m_current_time, model.id().id, snapshot_data);
	if (topmost_saved)
		// Restore the "saved" timestamp.
		m_saved_snapshot_time = m_current_time;
	m_active_snapshot_time = ++ m_current_time;
	// Save snapshot info of the last "current" aka "top most" state, that is only being serialized
	// if undoing an action. Such a snapshot has an invalid Model ID assigned if it was not taken yet.
	m_snapshots.emplace_back(topmost_snapshot_name, m_active_snapshot_time, 0, snapshot_data);
	// Release empty objects from the history.
	this->collect_garbage();
	assert(this->valid());
#ifdef SLIC3R_UNDOREDO_DEBUG
	std::cout << "After snapshot" << std::endl;
	this->print();
#endif /* SLIC3R_UNDOREDO_DEBUG */
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format("snapshot name %1%") % snapshot_name;
	plate_list.print();
}

void StackImpl::reduce_noisy_snapshots(const std::string& new_name)
{
	// Preceding snapshot must be a "leave gizmo" snapshot.
	assert(! m_snapshots.empty() && m_snapshots.back().is_topmost() && m_snapshots.back().timestamp == m_active_snapshot_time);
	auto it_last = m_snapshots.end();
	-- it_last; -- it_last;
	assert(it_last != m_snapshots.begin() && (it_last->snapshot_data.snapshot_type == SnapshotType::LeavingGizmoNoAction || it_last->snapshot_data.snapshot_type == SnapshotType::LeavingGizmoWithAction));
	if (it_last->snapshot_data.snapshot_type == SnapshotType::LeavingGizmoWithAction) {
		for (-- it_last; it_last->snapshot_data.snapshot_type != SnapshotType::EnteringGizmo; -- it_last) {
			if (it_last->snapshot_data.snapshot_type == SnapshotType::GizmoAction) {
                it_last->name = new_name;
                auto it = it_last;
				for (-- it; it->snapshot_data.snapshot_type == SnapshotType::GizmoAction; -- it) ;
				if (++ it < it_last) {
					// Drop (it, it_last>
					for (auto &kvp : m_objects)
						// Drop products of <it + 1, it_last + 1>
						kvp.second->release_between_timestamps(it->timestamp, (it_last + 1)->timestamp);
					it_last = this->release_snapshots(it + 1, it_last + 1);
				}
			}
			assert(it_last != m_snapshots.begin());
		}
	}
}

void StackImpl::load_snapshot(size_t timestamp, Slic3r::Model& model, Slic3r::GUI::GLGizmosManager& gizmos, Slic3r::GUI::PartPlateList& plate_list)
{
	// Find the snapshot by time. It must exist.
	const auto it_snapshot = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), Snapshot(timestamp));
	if (it_snapshot == m_snapshots.end() || it_snapshot->timestamp != timestamp)
		throw Slic3r::RuntimeError((boost::format("Snapshot with timestamp %1% does not exist") % timestamp).str());

	m_active_snapshot_time = timestamp;
	// BBS: reuse objects for backup, objects should clear children before load them
	model.collect_reusable_objects(m_reusable_objects);
	// model.clear_objects();
	model.clear_materials();
	this->load_mutable_object<Slic3r::Model>(ObjectID(it_snapshot->model_id), model);
	model.update_links_bottom_up_recursive();
	m_selection.volumes_and_instances.clear();
	this->load_mutable_object<Selection>(m_selection.id(), m_selection);
    //gizmos.reset_all_states(); FIXME: is this really necessary? It is quite unpleasant for the gizmo undo/redo substack
    this->load_mutable_object<Slic3r::GUI::GLGizmosManager>(gizmos.id(), gizmos);
    // Sort the volumes so that we may use binary search.
	std::sort(m_selection.volumes_and_instances.begin(), m_selection.volumes_and_instances.end());
	m_active_snapshot_time = timestamp;

	//BBS:load the partplater related data
	{
		//Slic3r::GUI::PartPlateList& plate_list = GUI::wxGetApp().plater()->get_partplate_list();
		std::vector<bool> previous_slice_result;
		std::vector<std::string> previous_gcode_paths;
		plate_list.get_sliced_result(previous_slice_result, previous_gcode_paths);

		plate_list.reset(false);
		this->load_mutable_object<Slic3r::GUI::PartPlateList>(plate_list.id(), plate_list);
		plate_list.rebuild_plates_after_deserialize(previous_slice_result, previous_gcode_paths);
	}
	this->m_active_snapshot_time = timestamp;
	assert(this->valid());

	// BBS: reuse objects for backup
	for (auto o : m_reusable_objects) {
		delete o;
	}
	m_reusable_objects.clear();

	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format("snapshot name %1%") % it_snapshot->name;
	plate_list.print();
}

bool StackImpl::has_undo_snapshot() const
{
#ifndef NDEBUG
	if (!this->valid())
		return false;
#endif
	// BBS: undo-redo until modify record
	auto it = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), Snapshot(m_active_snapshot_time));
	for (auto it2 = m_snapshots.begin(); it2 != it; ++it2) {
		if (snapshot_modifies_project(*it2))
			return true;
	}
	return false;
}

bool StackImpl::has_undo_snapshot(size_t time_to_load) const
{
	return time_to_load < m_active_snapshot_time && std::binary_search(m_snapshots.begin(), m_snapshots.end(), Snapshot(time_to_load));
}

bool StackImpl::has_redo_snapshot() const
{
#ifndef NDEBUG
	if (!this->valid())
		return false;
#endif

	// BBS: undo-redo until modify record
	auto it = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), Snapshot(m_active_snapshot_time));
	for (; it != m_snapshots.end(); ++it) {
		if (snapshot_modifies_project(*it))
			return true;
	}
	return false;
}

bool StackImpl::undo(Slic3r::Model &model, const Slic3r::GUI::Selection &selection, Slic3r::GUI::GLGizmosManager &gizmos, Slic3r::GUI::PartPlateList& plate_list, const SnapshotData &snapshot_data, size_t time_to_load)
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":time_to_load %1%") % time_to_load;
	assert(this->valid());
	if (time_to_load == SIZE_MAX) {
		auto it_current = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), Snapshot(m_active_snapshot_time));
		if (-- it_current == m_snapshots.begin())
			return false;
		time_to_load = it_current->timestamp;
	}
	assert(time_to_load < m_active_snapshot_time);
	assert(std::binary_search(m_snapshots.begin(), m_snapshots.end(), Snapshot(time_to_load)));
	bool new_snapshot_taken = false;
	if (m_active_snapshot_time == m_snapshots.back().timestamp && ! m_snapshots.back().is_topmost_captured()) {
		// The current state is temporary. The current state needs to be captured to be redoable.
        this->take_snapshot(topmost_snapshot_name, model, selection, gizmos, plate_list, snapshot_data);
        // The line above entered another topmost_snapshot_name.
		assert(m_snapshots.back().is_topmost());
		assert(! m_snapshots.back().is_topmost_captured());
		// Pop it back, it is not needed as there is now a captured topmost state.
		m_snapshots.pop_back();
		// current_time was extended, but it should not cause any harm. Resetting it back may complicate the logic unnecessarily.
		//-- m_current_time;
		assert(m_snapshots.back().is_topmost());
		assert(m_snapshots.back().is_topmost_captured());
		new_snapshot_taken = true;
	}
    this->load_snapshot(time_to_load, model, gizmos, plate_list);
	if (new_snapshot_taken) {
		// Release old snapshots if the memory allocated due to capturing the top most state is excessive.
		// Don't release the snapshots here, release them first after the scene and background processing gets updated, as this will release some references
		// to the shared TriangleMeshes.
		//this->release_least_recently_used();
	}
#ifdef SLIC3R_UNDOREDO_DEBUG
	std::cout << "After undo" << std::endl;
 	this->print();
#endif /* SLIC3R_UNDOREDO_DEBUG */
	return true;
}

bool StackImpl::redo(Slic3r::Model& model, Slic3r::GUI::GLGizmosManager& gizmos, Slic3r::GUI::PartPlateList& plate_list, size_t time_to_load)
{
	BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(":time_to_load %1%") % time_to_load;
	assert(this->valid());
	if (time_to_load == SIZE_MAX) {
		auto it_current = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), Snapshot(m_active_snapshot_time));
		if (++ it_current == m_snapshots.end())
			return false;
		time_to_load = it_current->timestamp;
	}
	assert(time_to_load > m_active_snapshot_time);
	assert(std::binary_search(m_snapshots.begin(), m_snapshots.end(), Snapshot(time_to_load)));
    this->load_snapshot(time_to_load, model, gizmos, plate_list);
#ifdef SLIC3R_UNDOREDO_DEBUG
	std::cout << "After redo" << std::endl;
 	this->print();
#endif /* SLIC3R_UNDOREDO_DEBUG */
	return true;
}

// Release snapshots between begin and end. Only erases data from m_snapshots, not from m_objects!
// Updates m_saved_snapshot_time.
std::vector<Snapshot>::iterator StackImpl::release_snapshots(std::vector<Snapshot>::iterator begin, std::vector<Snapshot>::iterator end)
{
	assert(! m_snapshots.empty());
	assert(begin <= end);
	if (m_saved_snapshot_time != size_t(-1) && m_saved_snapshot_time >= begin->timestamp && (end == m_snapshots.end() || m_saved_snapshot_time < end->timestamp)) {
		assert(m_saved_snapshot_time <= m_snapshots.back().timestamp);
		auto it_saved = std::lower_bound(begin, end, Snapshot(m_saved_snapshot_time));
		assert(it_saved != m_snapshots.end() && it_saved->timestamp == m_saved_snapshot_time);
		auto it = it_saved;
		for (; it != begin && ! snapshot_modifies_project(*it); -- it) ;
		if (it == begin && ! snapshot_modifies_project(*it)) {
			// Found a snapshot before begin, which captures the same project state.
			m_saved_snapshot_time = (-- it)->timestamp;
		} else {
			auto it = it_saved;
			for (; it != end && ! snapshot_modifies_project(*it); ++ it) ;
			if (it == end && end != m_snapshots.end())
				// Found a snapshot after end, which captures the same project state.
				m_saved_snapshot_time = (-- it)->timestamp;
			else
				// State of the project is being lost. Indicate a "likely modified" project state until the project is saved again.
				m_saved_snapshot_time = size_t(-1);
		}
	}
	return m_snapshots.erase(begin, end);
}

void StackImpl::collect_garbage()
{
	// Purge objects with empty histories.
	for (auto it = m_objects.begin(); it != m_objects.end();) {
		if (it->second->empty()) {
			if (it->second->immutable_object_ptr() != nullptr)
				// Release the immutable object from the ptr to ObjectID map.
				m_shared_ptr_to_object_id.erase(it->second->immutable_object_ptr());
			it = m_objects.erase(it);
		} else
			++ it;
	}
}

void StackImpl::release_least_recently_used()
{
	assert(this->valid());
	size_t current_memsize = this->memsize();
#ifdef SLIC3R_UNDOREDO_DEBUG
	bool released = false;
#endif
	// First try to release the optional immutable data (for example the convex hulls),
	// or the shared vertices of triangle meshes.
	for (auto it = m_objects.begin(); current_memsize > m_memory_limit && it != m_objects.end();) {
		const void *ptr = it->second->immutable_object_ptr();
		size_t mem_released = it->second->release_optional();
		if (it->second->empty()) {
			if (ptr != nullptr)
				// Release the immutable object from the ptr to ObjectID map.
				m_shared_ptr_to_object_id.erase(ptr);
			mem_released += it->second->memsize();
			it = m_objects.erase(it);
		} else
			++ it;
		assert(current_memsize >= mem_released);
		if (current_memsize >= mem_released)
			current_memsize -= mem_released;
		else
			current_memsize = 0;
	}
	while (current_memsize > m_memory_limit && m_snapshots.size() >= 3) {
		// From which side to remove a snapshot?
		assert(m_snapshots.front().timestamp < m_active_snapshot_time);
		size_t mem_released = 0;
		if (m_snapshots[1].timestamp == m_active_snapshot_time) {
			// Remove the last snapshot.
#if 0
			for (auto it = m_objects.begin(); it != m_objects.end();) {
				mem_released += it->second->release_after_timestamp(m_snapshots.back().timestamp);
				if (it->second->empty()) {
					if (it->second->immutable_object_ptr() != nullptr)
						// Release the immutable object from the ptr to ObjectID map.
						m_shared_ptr_to_object_id.erase(it->second->immutable_object_ptr());
					mem_released += it->second->memsize();
					it = m_objects.erase(it);
				} else
					++ it;
			}
			m_snapshots.pop_back();
			m_snapshots.back().name = topmost_snapshot_name;
#else
			// Rather don't release the last snapshot as it will be very confusing to the user
			// as of why he cannot jump to the top most state. The Undo / Redo stack maximum size
			// should be set low enough to accomodate for the top most snapshot.
			break;
#endif
		} else {
			// FIX: reduce_noisy_snapshots
			if (m_snapshots.front().snapshot_data.snapshot_type == SnapshotType::EnteringGizmo) {
                auto it_last = m_snapshots.begin() + 1;
                if (it_last->snapshot_data.snapshot_type == SnapshotType::GizmoAction) {
                    auto it = m_snapshots.begin();
                    // Drop (it, it_last>
                    for (auto &kvp : m_objects)
                        // Drop products of <it + 1, it_last + 1>
                        mem_released += kvp.second->release_between_timestamps(it->timestamp, (it_last + 1)->timestamp);
                    m_snapshots.erase(it + 1, it_last + 1);
                    assert(current_memsize >= mem_released);
                    if (current_memsize >= mem_released)
                        current_memsize -= mem_released;
                    else
                        current_memsize = 0;
					continue;
                }
				if (it_last->snapshot_data.snapshot_type != SnapshotType::LeavingGizmoWithAction &&
                       it_last->snapshot_data.snapshot_type != SnapshotType::LeavingGizmoNoAction)
					break;
            }
			// Remove the first snapshot.
			for (auto it = m_objects.begin(); it != m_objects.end();) {
				mem_released += it->second->release_before_timestamp(m_snapshots[1].timestamp);
				if (it->second->empty()) {
					if (it->second->immutable_object_ptr() != nullptr)
						// Release the immutable object from the ptr to ObjectID map.
						m_shared_ptr_to_object_id.erase(it->second->immutable_object_ptr());
					mem_released += it->second->memsize();
					it = m_objects.erase(it);
				} else
					++ it;
			}
			//FIXME update the "saved" snapshot time. DONE
            if (m_snapshots.front().timestamp == m_saved_snapshot_time)
				m_saved_snapshot_time = size_t(-1);
			m_snapshots.erase(m_snapshots.begin());
		}
		assert(current_memsize >= mem_released);
		if (current_memsize >= mem_released)
			current_memsize -= mem_released;
		else
			current_memsize = 0;
#ifdef SLIC3R_UNDOREDO_DEBUG
		released = true;
#endif
	}
	assert(this->valid());
#ifdef SLIC3R_UNDOREDO_DEBUG
	std::cout << "After release_least_recently_used" << std::endl;
 	this->print();
#endif /* SLIC3R_UNDOREDO_DEBUG */
}

bool StackImpl::project_modified() const
{
	assert(! m_snapshots.empty());

	if (m_saved_snapshot_time == size_t(-1))
		// Don't know anything about the project state.
		return true;
	if (m_saved_snapshot_time == m_active_snapshot_time)
		// Just saved at this step.
		return false;

	assert(m_saved_snapshot_time >= m_snapshots.front().timestamp && m_saved_snapshot_time <= m_snapshots.back().timestamp);

	auto it_saved = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), Snapshot(m_saved_snapshot_time));
	assert(it_saved != m_snapshots.end() && it_saved->timestamp == m_saved_snapshot_time);

#ifndef NDEBUG
	// Verify that there is a snapshot with "current time".
	auto it_current = std::lower_bound(m_snapshots.begin(), m_snapshots.end(), Snapshot(m_active_snapshot_time));
	assert(it_current != m_snapshots.end() && it_current->timestamp == m_active_snapshot_time);
#endif // NDEBUG

	if (m_saved_snapshot_time < m_active_snapshot_time) {
		// Search upwards. Ignore state of the "active" snapshot.
		for (auto it = it_saved; it->timestamp < m_active_snapshot_time; ++ it)
			if (snapshot_modifies_project(*it))
				return true;
	} else {
		// Search downwards. Ignore state of the "saved" snapshot.
		assert(m_saved_snapshot_time > m_active_snapshot_time);
		for (auto it = it_saved - 1; it->timestamp >= m_active_snapshot_time; -- it)
			if (snapshot_modifies_project(*it))
				return true;
	}

	return false;
}

// BBS: check if modify, skip snapshot with '!' ended name
bool StackImpl::has_real_change_from(size_t time) const
{
    if (m_active_snapshot_time == time) return false;
    auto it_time = std::lower_bound(m_snapshots.begin(),
                                              m_snapshots.end(),
                                              Snapshot(time));
    if (it_time == m_snapshots.end()) return true;
    auto it_active = std::lower_bound(m_snapshots.begin(),
                                              m_snapshots.end(),
                                      Snapshot(m_active_snapshot_time));
    if (it_active == m_snapshots.end()) return true;
    if (it_active > it_time) {
        for (; it_time < it_active; ++it_time) {
            if (snapshot_modifies_project(*it_time))
                return true;
		}
    } else {
        for (; it_active < it_time; ++it_active) {
            if (snapshot_modifies_project(*it_active))
                return true;
        }
	}
    return false;
}

// Wrappers of the private implementation.
Stack::Stack() : pimpl(new StackImpl()) {}
Stack::~Stack() {}
void Stack::clear() { pimpl->clear(); }
bool Stack::empty() const { return pimpl->empty(); }

void Stack::set_memory_limit(size_t memsize) { pimpl->set_memory_limit(memsize); }
size_t Stack::get_memory_limit() const { return pimpl->get_memory_limit(); }
size_t Stack::memsize() const { return pimpl->memsize(); }
void Stack::release_least_recently_used() { pimpl->release_least_recently_used(); }
void Stack::take_snapshot(const std::string& snapshot_name, const Slic3r::Model& model, const Slic3r::GUI::Selection& selection, const Slic3r::GUI::GLGizmosManager& gizmos, const SnapshotData &snapshot_data)
	{ pimpl->take_snapshot(snapshot_name, model, selection, gizmos, snapshot_data); }
void Stack::take_snapshot(const std::string& snapshot_name, const Slic3r::Model& model, const Slic3r::GUI::Selection& selection, const Slic3r::GUI::GLGizmosManager& gizmos, const Slic3r::GUI::PartPlateList& plate_list, const SnapshotData& snapshot_data)
	{ pimpl->take_snapshot(snapshot_name, model, selection, gizmos, plate_list, snapshot_data); }
void Stack::reduce_noisy_snapshots(const std::string& new_name) { pimpl->reduce_noisy_snapshots(new_name); }
bool Stack::has_undo_snapshot() const { return pimpl->has_undo_snapshot(); }
bool Stack::has_undo_snapshot(size_t time_to_load) const { return pimpl->has_undo_snapshot(time_to_load); }
bool Stack::has_redo_snapshot() const { return pimpl->has_redo_snapshot(); }
bool Stack::undo(Slic3r::Model& model, const Slic3r::GUI::Selection& selection, Slic3r::GUI::GLGizmosManager& gizmos, Slic3r::GUI::PartPlateList& plate_list, const SnapshotData &snapshot_data, size_t time_to_load)
	{ return pimpl->undo(model, selection, gizmos, plate_list, snapshot_data, time_to_load); }
bool Stack::redo(Slic3r::Model& model, Slic3r::GUI::GLGizmosManager& gizmos, Slic3r::GUI::PartPlateList& plate_list, size_t time_to_load) { return pimpl->redo(model, gizmos, plate_list, time_to_load); }
const Selection& Stack::selection_deserialized() const { return pimpl->selection_deserialized(); }

const std::vector<Snapshot>& Stack::snapshots() const { return pimpl->snapshots(); }
const Snapshot& Stack::snapshot(size_t time) const { return pimpl->snapshot(time); }
size_t Stack::active_snapshot_time() const { return pimpl->active_snapshot_time(); }
bool Stack::temp_snapshot_active() const { return pimpl->temp_snapshot_active(); }
void Stack::mark_current_as_saved() { pimpl->mark_current_as_saved(); }
bool Stack::project_modified() const { return pimpl->project_modified(); }

bool Stack::has_real_change_from(size_t time) const
{
    return pimpl->has_real_change_from(time);
}

} // namespace UndoRedo
} // namespace Slic3r
