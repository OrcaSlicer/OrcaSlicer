#ifndef slic3r_MutablePriorityQueue_hpp_
#define slic3r_MutablePriorityQueue_hpp_

#include <assert.h>
#include <type_traits>
constexpr auto InvalidQueueID = std::numeric_limits<size_t>::max();
template<typename T, typename IndexSetter, typename LessPredicate, const bool ResetIndexWhenRemoved = false>
class MutablePriorityQueue
{
public:
	static_assert(std::is_trivially_copyable<T>::value, "Template argument T must be a trivially copiable type in class template MutablePriorityQueue");

	// It is recommended to use make_mutable_priority_queue() for construction.
	MutablePriorityQueue(IndexSetter &&index_setter, LessPredicate &&less_predicate) :
		m_index_setter(std::forward<IndexSetter>(index_setter)), 
		m_less_predicate(std::forward<LessPredicate>(less_predicate)) 
		{}
	~MutablePriorityQueue()	{ clear(); }

	void		clear();
	void		reserve(size_t cnt) 				{ m_heap.reserve(cnt); }
	void		push(const T &item);
	void		push(T &&item);
	void		pop();
	T&			top()								{ return m_heap.front(); }
	void		remove(size_t idx);
	void		update(size_t idx) 					{ T item = m_heap[idx]; remove(idx); push(item); }

	size_t		size() const						{ return m_heap.size(); }
	bool		empty() const						{ return m_heap.empty(); }
	T&			operator[](std::size_t idx) noexcept { return m_heap[idx]; }
	const T&	operator[](std::size_t idx) const noexcept { return m_heap[idx]; }

	using iterator		 = typename std::vector<T>::iterator;
	using const_iterator = typename std::vector<T>::const_iterator;
	iterator 		begin() 		{ return m_heap.begin(); }
	iterator 		end() 			{ return m_heap.end(); }
	const_iterator 	cbegin() const	{ return m_heap.cbegin(); }
	const_iterator 	cend() const	{ return m_heap.cend(); }

protected:
	void		update_heap_up(size_t top, size_t bottom);
	void		update_heap_down(size_t top, size_t bottom);

private:
	std::vector<T>	m_heap;
	IndexSetter		m_index_setter;
	LessPredicate	m_less_predicate;
};

template<typename T, const bool ResetIndexWhenRemoved, typename IndexSetter, typename LessPredicate>
MutablePriorityQueue<T, IndexSetter, LessPredicate, ResetIndexWhenRemoved> make_mutable_priority_queue(IndexSetter &&index_setter, LessPredicate &&less_predicate)
{
    return MutablePriorityQueue<T, IndexSetter, LessPredicate, ResetIndexWhenRemoved>(
    	std::forward<IndexSetter>(index_setter), std::forward<LessPredicate>(less_predicate));
}

template<class T, class LessPredicate, class IndexSetter, const bool ResetIndexWhenRemoved>
inline void MutablePriorityQueue<T, LessPredicate, IndexSetter, ResetIndexWhenRemoved>::clear()
{ 
#ifdef NDEBUG
	// Only mark as removed from the queue in release mode, if configured so.
	if (ResetIndexWhenRemoved)
#endif /* NDEBUG */
	{
		for (size_t idx = 0; idx < m_heap.size(); ++ idx)
			// Mark as removed from the queue.
			m_index_setter(m_heap[idx], std::numeric_limits<size_t>::max());
	}
	m_heap.clear();
}

template<class T, class LessPredicate, class IndexSetter, const bool ResetIndexWhenRemoved>
inline void MutablePriorityQueue<T, LessPredicate, IndexSetter, ResetIndexWhenRemoved>::push(const T &item)
{
	size_t idx = m_heap.size();
	m_heap.emplace_back(item);
	m_index_setter(m_heap.back(), idx);
	update_heap_up(0, idx);
}

template<class T, class LessPredicate, class IndexSetter, const bool ResetIndexWhenRemoved>
inline void MutablePriorityQueue<T, LessPredicate, IndexSetter, ResetIndexWhenRemoved>::push(T &&item)
{
	size_t idx = m_heap.size();
	m_heap.emplace_back(std::move(item));
	m_index_setter(m_heap.back(), idx);
	update_heap_up(0, idx);
}

template<class T, class LessPredicate, class IndexSetter, const bool ResetIndexWhenRemoved>
inline void MutablePriorityQueue<T, LessPredicate, IndexSetter, ResetIndexWhenRemoved>::pop()
{
	assert(! m_heap.empty());
#ifdef NDEBUG
	// Only mark as removed from the queue in release mode, if configured so.
	if (ResetIndexWhenRemoved)
#endif /* NDEBUG */
	{
		// Mark as removed from the queue.
		m_index_setter(m_heap.front(), std::numeric_limits<size_t>::max());
	}
	if (m_heap.size() > 1) {
		m_heap.front() = m_heap.back();
		m_heap.pop_back();
		m_index_setter(m_heap.front(), 0);
		update_heap_down(0, m_heap.size() - 1);
	} else
		m_heap.clear();
}

template<class T, class LessPredicate, class IndexSetter, const bool ResetIndexWhenRemoved>
inline void MutablePriorityQueue<T, LessPredicate, IndexSetter, ResetIndexWhenRemoved>::remove(size_t idx)
{
	assert(idx < m_heap.size());
#ifdef NDEBUG
	// Only mark as removed from the queue in release mode, if configured so.
	if (ResetIndexWhenRemoved)
#endif /* NDEBUG */
	{
		// Mark as removed from the queue.
		m_index_setter(m_heap[idx], std::numeric_limits<size_t>::max());
	}
	if (idx + 1 == m_heap.size()) {
		m_heap.pop_back();
		return;
	}
	m_heap[idx] = m_heap.back();
	m_index_setter(m_heap[idx], idx);
	m_heap.pop_back();
	update_heap_down(idx, m_heap.size() - 1);
	update_heap_up(0, idx);
}

template<class T, class LessPredicate, class IndexSetter, const bool ResetIndexWhenRemoved>
inline void MutablePriorityQueue<T, LessPredicate, IndexSetter, ResetIndexWhenRemoved>::update_heap_up(size_t top, size_t bottom)
{
	size_t childIdx = bottom;
	T *child = &m_heap[childIdx];
	for (;;) {
		size_t parentIdx = (childIdx - 1) >> 1;
		if (childIdx == 0 || parentIdx < top)
			break;
		T *parent = &m_heap[parentIdx];
		// switch nodes
		if (! m_less_predicate(*parent, *child)) {
			T tmp = *parent;
			m_index_setter(tmp,    childIdx);
			m_index_setter(*child, parentIdx);
			m_heap[parentIdx] = *child;
			m_heap[childIdx]  = tmp;
		}
		// shift up
		childIdx = parentIdx;
		child = parent;
	}
}

template<class T, class LessPredicate, class IndexSetter, const bool ResetIndexWhenRemoved>
inline void MutablePriorityQueue<T, LessPredicate, IndexSetter, ResetIndexWhenRemoved>::update_heap_down(size_t top, size_t bottom)
{
	size_t parentIdx = top;
	T *parent = &m_heap[parentIdx];
	for (;;) {
		size_t childIdx = (parentIdx << 1) + 1;
		if (childIdx > bottom)
			break;
		T *child = &m_heap[childIdx];
		size_t child2Idx = childIdx + 1;
		if (child2Idx <= bottom) {
			T *child2 = &m_heap[child2Idx];
			if (! m_less_predicate(*child, *child2)) {
				child = child2;
				childIdx = child2Idx;
			}
		}
		if (m_less_predicate(*parent, *child))
			return;
		// switch nodes
		T tmp = *parent;
		m_index_setter(tmp,    childIdx);
		m_index_setter(*child, parentIdx);
		m_heap[parentIdx] = *child;
		m_heap[childIdx] = tmp;
		// shift down
		parentIdx = childIdx;
		parent = child;
	}
}

// Binary heap addressing of a hierarchy of binary miniheaps by a higher level binary heap.
// Conceptually it works the same as a plain binary heap, however it is cache friendly.
// A binary block of "block_size" implements a binary miniheap of (block_size / 2) leaves and 
// ((block_size / 2) - 1) nodes, thus wasting a single element. To make addressing simpler,
// the zero'th element inside each miniheap is wasted, thus for example a single element heap is
// 2 elements long and the 1st element starts at address 1.
//
// Mostly copied from the following great source:
// https://playfulprogramming.blogspot.com/2015/08/cache-optimizing-priority-queue.html
// https://github.com/rollbear/prio_queue/blob/master/prio_queue.hpp
// original source Copyright Björn Fahller 2015, Boost Software License, Version 1.0, http://www.boost.org/LICENSE_1_0.txt
template <std::size_t blocking>
struct SkipHeapAddressing
{
public:
	static const constexpr std::size_t block_size = blocking;
	static const constexpr std::size_t block_mask = block_size - 1;
	static_assert((block_size & block_mask) == 0U, "block size must be 2^n for some integer n");

	static inline std::size_t child_of(std::size_t node_no) noexcept {
  		if (! is_block_leaf(node_no))
  			// If not a leaf, then it is sufficient to just traverse down inside a miniheap.
  			// The following line is equivalent to, but quicker than
  			// return block_base(node_no) + 2 * block_offset(node_no);
			return node_no + block_offset(node_no);
		// Otherwise skip to a root of a child miniheap.
		return (block_base(node_no) + 1 + child_no(node_no) * 2) * block_size + 1;
	}

	static inline std::size_t parent_of(std::size_t node_no) noexcept {
  		auto const node_root = block_base(node_no); // 16
  		if (! is_block_root(node_no))
  			// If not a block (miniheap) root, then it is sufficient to just traverse up inside a miniheap.
			return node_root + block_offset(node_no) / 2;
		// Otherwise skipping from a root of one miniheap into leaf of another miniheap.
		// Address of a parent miniheap block. One miniheap branches at (block_size / 2) leaves to (block_size) miniheaps.
		auto const parent_base = block_base(node_root / block_size - 1); // 0
		// Index of a leaf of a parent miniheap, which is a parent of node_no.
		auto const child       = ((node_no - block_size) / block_size - parent_base) / 2;
		return 
			// Address of a parent miniheap
			parent_base + 
			// Address of a leaf of a parent miniheap
			block_size / 2 + child; // 30
	}

	// Leafs are stored inside the second half of a block.
	static inline bool 			is_block_leaf(std::size_t node_no) noexcept { return (node_no & (block_size >> 1)) != 0U; }
	// Unused space aka padding to facilitate quick addressing.
	static inline bool 			is_padding   (std::size_t node_no) noexcept { return block_offset(node_no) == 0U; }
// Following methods are internal, but made public for unit tests.
//private:
	// Address is a root of a block (of a miniheap).
	static inline bool 			is_block_root(std::size_t node_no) noexcept { return block_offset(node_no) == 1U; }
	// Offset inside a block (inside a miniheap).
	static inline std::size_t 	block_offset (std::size_t node_no) noexcept { return node_no & block_mask; }
	// Base address of a block (a miniheap).
	static inline std::size_t 	block_base   (std::size_t node_no) noexcept { return node_no & ~block_mask; }
	// Index of a leaf.
	static inline std::size_t 	child_no     (std::size_t node_no) noexcept { assert(is_block_leaf(node_no)); return node_no & (block_mask >> 1); }
};

// Cache friendly variant of MutablePriorityQueue, implemented as a binary heap of binary miniheaps,
// building upon SkipHeapAddressing.
template<typename T, typename IndexSetter, typename LessPredicate, std::size_t blocking = 32, const bool ResetIndexWhenRemoved = false>
class MutableSkipHeapPriorityQueue
{
public:
	static_assert(std::is_trivially_copyable<T>::value, "Template argument T must be a trivially copiable type in class template MutableSkipHeapPriorityQueue");
	using address = SkipHeapAddressing<blocking>;

	// It is recommended to use make_miniheap_mutable_priority_queue() for construction.
	MutableSkipHeapPriorityQueue(IndexSetter &&index_setter, LessPredicate &&less_predicate) :
		m_index_setter(std::forward<IndexSetter>(index_setter)), 
		m_less_predicate(std::forward<LessPredicate>(less_predicate)) 
		{}
	~MutableSkipHeapPriorityQueue()	{ clear(); }

	void		clear();
	// Reserve one unused element per miniheap.
	void		reserve(size_t cnt) 				{ m_heap.reserve(cnt + ((cnt + (address::block_size - 1)) / (address::block_size - 1))); }
	void		push(const T &item);
	void		push(T &&item);
	void		pop();
	T&			top()								{ return m_heap[1]; }
	void		remove(size_t idx);
	void		update(size_t idx) 					{ assert(! address::is_padding(idx)); T item = m_heap[idx]; remove(idx); push(item); }
	// There is one padding element storead at each miniheap, thus lower the number of elements by the number of miniheaps.
	size_t 		size() const noexcept 				{ return m_heap.size() - (m_heap.size() + address::block_size - 1) / address::block_size; }
	bool		empty() const						{ return m_heap.empty(); }
	T&			operator[](std::size_t idx) noexcept { assert(! address::is_padding(idx)); return m_heap[idx]; }
	const T&    operator[](std::size_t idx) const noexcept { assert(! address::is_padding(idx)); return m_heap[idx]; }

protected:
	void		update_heap_up(size_t top, size_t bottom);
	void		update_heap_down(size_t top, size_t bottom);
	void   		pop_back() noexcept {
		assert(m_heap.size() > 1);
		assert(! address::is_padding(m_heap.size() - 1));
		m_heap.pop_back();
		if (address::is_padding(m_heap.size() - 1))
			m_heap.pop_back();
	}

private:
	std::vector<T>	m_heap;
	IndexSetter		m_index_setter;
	LessPredicate	m_less_predicate;
};

template<typename T, std::size_t BlockSize, const bool ResetIndexWhenRemoved, typename IndexSetter, typename LessPredicate>
MutableSkipHeapPriorityQueue<T, IndexSetter, LessPredicate, BlockSize, ResetIndexWhenRemoved> 
	make_miniheap_mutable_priority_queue(IndexSetter &&index_setter, LessPredicate &&less_predicate)
{
    return MutableSkipHeapPriorityQueue<T, IndexSetter, LessPredicate, BlockSize, ResetIndexWhenRemoved>(
    	std::forward<IndexSetter>(index_setter), std::forward<LessPredicate>(less_predicate));
}

template<class T, class LessPredicate, class IndexSetter, std::size_t blocking, const bool ResetIndexWhenRemoved>
inline void MutableSkipHeapPriorityQueue<T, LessPredicate, IndexSetter, blocking, ResetIndexWhenRemoved>::clear()
{ 
#ifdef NDEBUG
	// Only mark as removed from the queue in release mode, if configured so.
	if (ResetIndexWhenRemoved)
#endif /* NDEBUG */
	{
		for (size_t idx = 0; idx < m_heap.size(); ++ idx)
			// Mark as removed from the queue.
			if (! address::is_padding(idx))
				m_index_setter(m_heap[idx], std::numeric_limits<size_t>::max());
	}
	m_heap.clear();
}

template<class T, class LessPredicate, class IndexSetter, std::size_t blocking, const bool ResetIndexWhenRemoved>
inline void MutableSkipHeapPriorityQueue<T, LessPredicate, IndexSetter, blocking, ResetIndexWhenRemoved>::push(const T &item)
{
	if (address::is_padding(m_heap.size()))
		m_heap.emplace_back(T());
	size_t idx = m_heap.size();
	m_heap.emplace_back(item);
	m_index_setter(m_heap.back(), idx);
	update_heap_up(1, idx);
}

template<class T, class LessPredicate, class IndexSetter, std::size_t blocking, const bool ResetIndexWhenRemoved>
inline void MutableSkipHeapPriorityQueue<T, LessPredicate, IndexSetter, blocking, ResetIndexWhenRemoved>::push(T &&item)
{
	if (address::is_padding(m_heap.size()))
		m_heap.emplace_back(T());
	size_t idx = m_heap.size();
	m_heap.emplace_back(std::move(item));
	m_index_setter(m_heap.back(), idx);
	update_heap_up(1, idx);
}

template<class T, class LessPredicate, class IndexSetter, std::size_t blocking, const bool ResetIndexWhenRemoved>
inline void MutableSkipHeapPriorityQueue<T, LessPredicate, IndexSetter, blocking, ResetIndexWhenRemoved>::pop()
{
	assert(! m_heap.empty());
#ifdef NDEBUG
	// Only mark as removed from the queue in release mode, if configured so.
	if (ResetIndexWhenRemoved)
#endif /* NDEBUG */
	{
		// Mark as removed from the queue.
		m_index_setter(m_heap[1], std::numeric_limits<size_t>::max());
	}
	// Zero'th element is padding, thus non-empty queue must have at least two elements.
	if (m_heap.size() > 2) {
		m_heap[1] = m_heap.back();
		this->pop_back();
		m_index_setter(m_heap[1], 1);
		update_heap_down(1, m_heap.size() - 1);
	} else
		m_heap.clear();
}

template<class T, class LessPredicate, class IndexSetter, std::size_t blocking, const bool ResetIndexWhenRemoved>
inline void MutableSkipHeapPriorityQueue<T, LessPredicate, IndexSetter, blocking, ResetIndexWhenRemoved>::remove(size_t idx)
{
	assert(idx < m_heap.size());
	assert(! address::is_padding(idx));
#ifdef NDEBUG
	// Only mark as removed from the queue in release mode, if configured so.
	if (ResetIndexWhenRemoved)
#endif /* NDEBUG */
	{
		// Mark as removed from the queue.
		m_index_setter(m_heap[idx], std::numeric_limits<size_t>::max());
	}
	if (idx + 1 == m_heap.size()) {
		this->pop_back();
		return;
	}
	m_heap[idx] = m_heap.back();
	m_index_setter(m_heap[idx], idx);
	this->pop_back();
	update_heap_down(idx, m_heap.size() - 1);
	update_heap_up(1, idx);
}

template<class T, class LessPredicate, class IndexSetter, std::size_t blocking, const bool ResetIndexWhenRemoved>
inline void MutableSkipHeapPriorityQueue<T, LessPredicate, IndexSetter, blocking, ResetIndexWhenRemoved>::update_heap_up(size_t top, size_t bottom)
{
	assert(! address::is_padding(top));
	assert(! address::is_padding(bottom));
	size_t childIdx = bottom;
	T *child = &m_heap[childIdx];
	for (;;) {
		size_t parentIdx = address::parent_of(childIdx);
		if (childIdx == 1 || parentIdx < top)
			break;
		T *parent = &m_heap[parentIdx];
		// switch nodes
		if (! m_less_predicate(*parent, *child)) {
			T tmp = *parent;
			m_index_setter(tmp,    childIdx);
			m_index_setter(*child, parentIdx);
			m_heap[parentIdx] = *child;
			m_heap[childIdx]  = tmp;
		}
		// shift up
		childIdx = parentIdx;
		child = parent;
	}
}

template<class T, class LessPredicate, class IndexSetter, std::size_t blocking, const bool ResetIndexWhenRemoved>
inline void MutableSkipHeapPriorityQueue<T, LessPredicate, IndexSetter, blocking, ResetIndexWhenRemoved>::update_heap_down(size_t top, size_t bottom)
{
	assert(! address::is_padding(top));
	assert(! address::is_padding(bottom));
	size_t parentIdx = top;
	T *parent = &m_heap[parentIdx];
	for (;;) {
		size_t childIdx = address::child_of(parentIdx);
		if (childIdx > bottom)
			break;
		T *child = &m_heap[childIdx];
		size_t child2Idx = childIdx + (address::is_block_leaf(parentIdx) ? address::block_size : 1);
		if (child2Idx <= bottom) {
			T *child2 = &m_heap[child2Idx];
			if (! m_less_predicate(*child, *child2)) {
				child = child2;
				childIdx = child2Idx;
			}
		}
		if (m_less_predicate(*parent, *child))
			return;
		// switch nodes
		T tmp = *parent;
		m_index_setter(tmp,    childIdx);
		m_index_setter(*child, parentIdx);
		m_heap[parentIdx] = *child;
		m_heap[childIdx]  = tmp;
		// shift down
		parentIdx = childIdx;
		parent = child;
	}
}

#endif /* slic3r_MutablePriorityQueue_hpp_ */
