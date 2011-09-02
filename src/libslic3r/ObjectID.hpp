#ifndef slic3r_ObjectID_hpp_
#define slic3r_ObjectID_hpp_

#include <cereal/access.hpp>
#include <cereal/types/base_class.hpp>

namespace Slic3r {

namespace UndoRedo {
	class StackImpl;
};

// Unique identifier of a mutable object accross the application.
// Used to synchronize the front end (UI) with the back end (BackgroundSlicingProcess / Print / PrintObject)
// (for Model, ModelObject, ModelVolume, ModelInstance or ModelMaterial classes)
// and to serialize / deserialize an object onto the Undo / Redo stack.
// Valid IDs are strictly positive (non zero).
// It is declared as an object, as some compilers (notably msvcc) consider a typedef size_t equivalent to size_t
// for parameter overload.
class ObjectID
{
public:
	ObjectID(size_t id) : id(id) {}
	// Default constructor constructs an invalid ObjectID.
	ObjectID() : id(0) {}

	bool operator==(const ObjectID &rhs) const { return this->id == rhs.id; }
	bool operator!=(const ObjectID &rhs) const { return this->id != rhs.id; }
	bool operator< (const ObjectID &rhs) const { return this->id <  rhs.id; }
	bool operator> (const ObjectID &rhs) const { return this->id >  rhs.id; }
	bool operator<=(const ObjectID &rhs) const { return this->id <= rhs.id; }
	bool operator>=(const ObjectID &rhs) const { return this->id >= rhs.id; }

    bool valid() const { return id != 0; }
    bool invalid() const { return id == 0; }

	size_t	id;

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(id); }
};

// Base for Model, ModelObject, ModelVolume, ModelInstance or ModelMaterial to provide a unique ID
// to synchronize the front end (UI) with the back end (BackgroundSlicingProcess / Print / PrintObject).
// Also base for Print, PrintObject, SLAPrint, SLAPrintObject to provide a unique ID for matching Model / ModelObject
// with their corresponding Print / PrintObject objects by the notification center at the UI when processing back-end warnings.
// Achtung! The s_last_id counter is not thread safe, so it is expected, that the ObjectBase derived instances
// are only instantiated from the main thread.
class ObjectBase
{
public:
    using Timestamp = uint64_t;

    ObjectID     		id() const { return m_id; }
    // Return an optional timestamp of this object.
    // If the timestamp returned is non-zero, then the serialization framework will
    // only save this object on the Undo/Redo stack if the timestamp is different
    // from the timestmap of the object at the top of the Undo / Redo stack.
    virtual Timestamp	timestamp() const { return 0; }

protected:
    // Constructors to be only called by derived classes.
    // Default constructor to assign a unique ID.
    ObjectBase() : m_id(generate_new_id()) {}
    // Constructor with ignored int parameter to assign an invalid ID, to be replaced
    // by an existing ID copied from elsewhere.
    ObjectBase(int) : m_id(ObjectID(0)) {}

    ObjectBase(const ObjectID id) : m_id(id) {}
	// The class tree will have virtual tables and type information.
	virtual ~ObjectBase() = default;

    // Use with caution!
    void        set_new_unique_id() { m_id = generate_new_id(); }
    void        set_invalid_id()    { m_id = 0; }
    // Use with caution!
    void        copy_id(const ObjectBase &rhs) { m_id = rhs.id(); }

    // Override this method if a ObjectBase derived class owns other ObjectBase derived instances.
    virtual void assign_new_unique_ids_recursive() { this->set_new_unique_id(); }

private:
    ObjectID                m_id;

	static inline ObjectID  generate_new_id() { return ObjectID(++ s_last_id); }
    static size_t           s_last_id;
	
	friend ObjectID wipe_tower_object_id();
	friend ObjectID wipe_tower_instance_id();

	friend class cereal::access;
	friend class Slic3r::UndoRedo::StackImpl;
	template<class Archive> void serialize(Archive &ar) { ar(m_id); }
  	template<class Archive> static void load_and_construct(Archive & ar, cereal::construct<ObjectBase> &construct) { ObjectID id; ar(id); construct(id); }
};

class ObjectWithTimestamp : public ObjectBase
{
protected:
    // Constructors to be only called by derived classes.
    // Default constructor to assign a new timestamp unique to this object's history.
	ObjectWithTimestamp() = default;
    // Constructor with ignored int parameter to assign an invalid ID, to be replaced
    // by an existing ID copied from elsewhere.
    ObjectWithTimestamp(int) : ObjectBase(-1) {}
	// The class tree will have virtual tables and type information.
	virtual ~ObjectWithTimestamp() = default;

    // The timestamp uniquely identifies content of the derived class' data, therefore it makes sense to copy the timestamp if the content data was copied.
    void                copy_timestamp(const ObjectWithTimestamp& rhs) { m_timestamp = rhs.m_timestamp; }

public:
    // Return an optional timestamp of this object.
    // If the timestamp returned is non-zero, then the serialization framework will
    // only save this object on the Undo/Redo stack if the timestamp is different
    // from the timestmap of the object at the top of the Undo / Redo stack.
    Timestamp	        timestamp() const throw() override { return m_timestamp; }
    bool 				timestamp_matches(const ObjectWithTimestamp &rhs) const throw() { return m_timestamp == rhs.m_timestamp; }
    bool 				object_id_and_timestamp_match(const ObjectWithTimestamp &rhs) const throw() { return this->id() == rhs.id() && m_timestamp == rhs.m_timestamp; }
    void 				touch() { m_timestamp = ++ s_last_timestamp; }

private:
	// The first timestamp is non-zero, as zero timestamp means the timestamp is not reliable.
	Timestamp 			m_timestamp { 1 };
    static Timestamp    s_last_timestamp;
	
	friend class cereal::access;
	friend class Slic3r::UndoRedo::StackImpl;
	template<class Archive> void serialize(Archive &ar) { ar(m_timestamp); }
};

class CutObjectBase : public ObjectBase
{
    // check sum of CutParts in initial Object
    size_t m_check_sum{1};
    // connectors count
    size_t m_connectors_cnt{0};

public:
    // Default Constructor to assign an invalid ID
    CutObjectBase() : ObjectBase(-1) {}
    // Constructor with ignored int parameter to assign an invalid ID, to be replaced
    // by an existing ID copied from elsewhere.
    CutObjectBase(int) : ObjectBase(-1) {}
    // Constructor to initialize full information from 3mf
    CutObjectBase(ObjectID id, size_t check_sum, size_t connectors_cnt) : ObjectBase(id), m_check_sum(check_sum), m_connectors_cnt(connectors_cnt) {}
    // The class tree will have virtual tables and type information.
    virtual ~CutObjectBase() = default;

    bool operator<(const CutObjectBase &other) const { return other.id() > this->id(); }
    bool operator==(const CutObjectBase &other) const { return other.id() == this->id(); }

    void copy(const CutObjectBase &rhs)
    {
        this->copy_id(rhs);
        this->m_check_sum      = rhs.check_sum();
        this->m_connectors_cnt = rhs.connectors_cnt();
    }
    CutObjectBase &operator=(const CutObjectBase &other)
    {
        this->copy(other);
        return *this;
    }

    void invalidate()
    {
        set_invalid_id();
        m_check_sum      = 1;
        m_connectors_cnt = 0;
    }

    void init() { this->set_new_unique_id(); }
    bool has_same_id(const CutObjectBase &rhs) { return this->id() == rhs.id(); }
    bool is_equal(const CutObjectBase &rhs) { return this->id() == rhs.id() && this->check_sum() == rhs.check_sum() && this->connectors_cnt() == rhs.connectors_cnt(); }

    size_t check_sum() const { return m_check_sum; }
    void   set_check_sum(size_t cs) { m_check_sum = cs; }
    void   increase_check_sum(size_t cnt) { m_check_sum += cnt; }

    size_t connectors_cnt() const { return m_connectors_cnt; }
    void   increase_connectors_cnt(size_t connectors_cnt) { m_connectors_cnt += connectors_cnt; }

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar)
    {
        ar(cereal::base_class<ObjectBase>(this));
        ar(m_check_sum, m_connectors_cnt);
    }
};


// Unique object / instance ID for the wipe tower.
extern ObjectID wipe_tower_object_id();
extern ObjectID wipe_tower_instance_id();

} // namespace Slic3r

#endif /* slic3r_ObjectID_hpp_ */
