#if 0
	#pragma optimize("", off)
	#undef NDEBUG
	#undef assert
#endif

#include "clipper.hpp"
#include "ShortestPath.hpp"
#include "KDTreeIndirect.hpp"
#include "MutablePriorityQueue.hpp"
#include "Print.hpp"

#include <cmath>
#include <cassert>

namespace Slic3r {

// Naive implementation of the Traveling Salesman Problem, it works by always taking the next closest neighbor.
// This implementation will always produce valid result even if some segments cannot reverse.
template<typename EndPointType, typename KDTreeType, typename CouldReverseFunc>
std::vector<std::pair<size_t, bool>> chain_segments_closest_point(std::vector<EndPointType> &end_points, KDTreeType &kdtree, CouldReverseFunc &could_reverse_func, EndPointType &first_point)
{
	assert((end_points.size() & 1) == 0);
    size_t num_segments = end_points.size() / 2;
	assert(num_segments >= 2);
	for (EndPointType &ep : end_points)
		ep.chain_id = 0;
	std::vector<std::pair<size_t, bool>> out;
	out.reserve(num_segments);
	size_t first_point_idx = &first_point - end_points.data();
	out.emplace_back(first_point_idx / 2, (first_point_idx & 1) != 0);
	first_point.chain_id = 1;
	size_t this_idx = first_point_idx ^ 1;
	for (int iter = (int)num_segments - 2; iter >= 0; -- iter) {
		EndPointType &this_point = end_points[this_idx];
    	this_point.chain_id = 1;
    	// Find the closest point to this end_point, which lies on a different extrusion path (filtered by the lambda).
    	// Ignore the starting point as the starting point is considered to be occupied, no end point coud connect to it.
		size_t next_idx = find_closest_point(kdtree, this_point.pos,
			[this_idx, &end_points, &could_reverse_func](size_t idx) {
				return (idx ^ this_idx) > 1 && end_points[idx].chain_id == 0 && ((idx & 1) == 0 || could_reverse_func(idx >> 1));
		});
		assert(next_idx < end_points.size());
		EndPointType &end_point = end_points[next_idx];
		end_point.chain_id = 1;
		assert((next_idx & 1) == 0 || could_reverse_func(next_idx >> 1));
		out.emplace_back(next_idx / 2, (next_idx & 1) != 0);
		this_idx = next_idx ^ 1;
	}
#ifndef NDEBUG
	assert(end_points[this_idx].chain_id == 0);
	for (EndPointType &ep : end_points)
		assert(&ep == &end_points[this_idx] || ep.chain_id == 1);
#endif /* NDEBUG */
	return out;
}

// Chain perimeters (always closed) and thin fills (closed or open) using a greedy algorithm.
// Solving a Traveling Salesman Problem (TSP) with the modification, that the sites are not always points, but points and segments.
// Solving using a greedy algorithm, where a shortest edge is added to the solution if it does not produce a bifurcation or a cycle.
// Return index and "reversed" flag.
// https://en.wikipedia.org/wiki/Multi-fragment_algorithm
// The algorithm builds a tour for the traveling salesman one edge at a time and thus maintains multiple tour fragments, each of which 
// is a simple path in the complete graph of cities. At each stage, the algorithm selects the edge of minimal cost that either creates 
// a new fragment, extends one of the existing paths or creates a cycle of length equal to the number of cities.
template<typename PointType, typename SegmentEndPointFunc, bool REVERSE_COULD_FAIL, typename CouldReverseFunc>
std::vector<std::pair<size_t, bool>> chain_segments_greedy_constrained_reversals_(SegmentEndPointFunc end_point_func, CouldReverseFunc could_reverse_func, size_t num_segments, const PointType *start_near)
{
	std::vector<std::pair<size_t, bool>> out;

	if (num_segments == 0) {
		// Nothing to do.
	} 
	else if (num_segments == 1)
	{
		// Just sort the end points so that the first point visited is closest to start_near.
		out.emplace_back(0, could_reverse_func(0) && start_near != nullptr && 
            (end_point_func(0, false) - *start_near).template cast<double>().squaredNorm() < (end_point_func(0, true) - *start_near).template cast<double>().squaredNorm());
	} 
	else
	{
		// End points of segments for the KD tree closest point search.
		// A single end point is inserted into the search structure for loops, two end points are entered for open paths.
		struct EndPoint {
			EndPoint(const Vec2d &pos) : pos(pos) {}
			Vec2d     pos;
			// Identifier of the chain, to which this end point belongs. Zero means unassigned.
			size_t    chain_id = 0;
			// Link to the closest currently valid end point.
			EndPoint *edge_out = nullptr;
			// Distance to the next end point following the link.
			// Zero value -> start of the final path.
			double    distance_out = std::numeric_limits<double>::max();
			size_t    heap_idx = std::numeric_limits<size_t>::max();
		};
	    std::vector<EndPoint> end_points;
	    end_points.reserve(num_segments * 2);
	    for (size_t i = 0; i < num_segments; ++ i) {
            end_points.emplace_back(end_point_func(i, true ).template cast<double>());
            end_points.emplace_back(end_point_func(i, false).template cast<double>());
	    }

	    // Construct the closest point KD tree over end points of segments.
		auto coordinate_fn = [&end_points](size_t idx, size_t dimension) -> double { return end_points[idx].pos[dimension]; };
		KDTreeIndirect<2, double, decltype(coordinate_fn)> kdtree(coordinate_fn, end_points.size());

		// Helper to detect loops in already connected paths.
		// Unique chain IDs are assigned to paths. If paths are connected, end points will not have their chain IDs updated, but the chain IDs
		// will remember an "equivalent" chain ID, which is the lowest ID of all the IDs in the path, and the lowest ID is equivalent to itself.
		class EquivalentChains {
		public:
			// Zero'th chain ID is invalid.
			EquivalentChains(size_t reserve) { m_equivalent_with.reserve(reserve); m_equivalent_with.emplace_back(0); }
			// Generate next equivalence class.
			size_t 				next() {
				m_equivalent_with.emplace_back(++ m_last_chain_id);
				return m_last_chain_id;
			}
			// Get equivalence class for chain ID.
			size_t 				operator()(size_t chain_id) {
				if (chain_id != 0) {
					for (size_t last = chain_id;;) {
						size_t lower = m_equivalent_with[last];
						if (lower == last) {
							m_equivalent_with[chain_id] = lower;
							chain_id = lower;
							break;
						}
						last = lower;
					}
				}
				return chain_id;
			}
			size_t 			  	merge(size_t chain_id1, size_t chain_id2) {
				size_t chain_id = std::min((*this)(chain_id1), (*this)(chain_id2));
				m_equivalent_with[chain_id1] = chain_id;
				m_equivalent_with[chain_id2] = chain_id;
				return chain_id;
			}

#ifndef NDEBUG
			bool				validate()
			{
				assert(m_last_chain_id >= 0);
				assert(m_last_chain_id + 1 == m_equivalent_with.size());
				for (size_t i = 0; i < m_equivalent_with.size(); ++ i) {
					for (size_t last = i;;) {
						size_t lower = m_equivalent_with[last];
						assert(lower <= last);
						if (lower == last)
							break;
						last = lower;
					}
				}
				return true;
			}
#endif /* NDEBUG */

		private:
			// Unique chain ID assigned to chains of end points of segments.
			size_t              m_last_chain_id = 0;
			std::vector<size_t> m_equivalent_with;
		} equivalent_chain(num_segments);

		// Find the first end point closest to start_near.
		EndPoint *first_point = nullptr;
		size_t    first_point_idx = std::numeric_limits<size_t>::max();
		if (start_near != nullptr) {
            size_t idx = find_closest_point(kdtree, start_near->template cast<double>(),
				// Don't start with a reverse segment, if flipping of the segment is not allowed.
				[&could_reverse_func](size_t idx) { return (idx & 1) == 0 || could_reverse_func(idx >> 1); });
			assert(idx < end_points.size());
			first_point = &end_points[idx];
			first_point->distance_out = 0.;
			first_point->chain_id = equivalent_chain.next();
			first_point_idx = idx;
		}
		EndPoint *initial_point = first_point;
		EndPoint *last_point = nullptr;

		// Assign the closest point and distance to the end points.
		for (EndPoint &end_point : end_points) {
	    	assert(end_point.edge_out == nullptr);
	    	if (&end_point != first_point) {
		    	size_t this_idx = &end_point - &end_points.front();
		    	// Find the closest point to this end_point, which lies on a different extrusion path (filtered by the lambda).
		    	// Ignore the starting point as the starting point is considered to be occupied, no end point coud connect to it.
				size_t next_idx = find_closest_point(kdtree, end_point.pos, 
					[this_idx, first_point_idx](size_t idx){ return idx != first_point_idx && (idx ^ this_idx) > 1; });
				assert(next_idx < end_points.size());
				EndPoint &end_point2 = end_points[next_idx];
				end_point.edge_out = &end_point2;
				end_point.distance_out = (end_point2.pos - end_point.pos).squaredNorm();
			}
		}

	    // Initialize a heap of end points sorted by the lowest distance to the next valid point of a path.
	    auto queue = make_mutable_priority_queue<EndPoint*, false>(
			[](EndPoint *ep, size_t idx){ ep->heap_idx = idx; }, 
	    	[](EndPoint *l, EndPoint *r){ return l->distance_out < r->distance_out; });
		queue.reserve(end_points.size() * 2 - 1);
	    for (EndPoint &ep : end_points)
	    	if (first_point != &ep)
				queue.push(&ep);

#ifndef NDEBUG
		auto validate_graph_and_queue = [&equivalent_chain, &end_points, &queue, first_point]() -> bool {
			assert(equivalent_chain.validate());
			for (EndPoint &ep : end_points) {
				if (ep.heap_idx < queue.size()) {
					// End point is on the heap.
					assert(*(queue.cbegin() + ep.heap_idx) == &ep);
					assert(ep.chain_id == 0);
				} else {
					// End point is NOT on the heap, therefore it is part of the output path.
					assert(ep.heap_idx == std::numeric_limits<size_t>::max());
					assert(ep.chain_id != 0);
					if (&ep == first_point) {
						assert(ep.edge_out == nullptr);
					} else {
						assert(ep.edge_out != nullptr);
						// Detect loops.
						for (EndPoint *pt = &ep; pt != nullptr;) {
							// Out of queue. It is a final point.
							assert(pt->heap_idx == std::numeric_limits<size_t>::max());
							EndPoint *pt_other = &end_points[(pt - &end_points.front()) ^ 1];
							if (pt_other->heap_idx < queue.size())
								// The other side of this segment is undecided yet.
								break;
							pt = pt_other->edge_out;
						}
					}
				}
			}
			for (EndPoint *ep : queue)
				// Points in the queue are not connected yet.
				assert(ep->chain_id == 0);
			return true;
		};
#endif /* NDEBUG */

	    // Chain the end points: find (num_segments - 1) shortest links not forming bifurcations or loops.
		assert(num_segments >= 2);
#ifndef NDEBUG
		double distance_taken_last = 0.;
#endif /* NDEBUG */
		for (int iter = int(num_segments) - 2;; -- iter) {
			assert(validate_graph_and_queue());
	    	// Take the first end point, for which the link points to the currently closest valid neighbor.
	    	EndPoint &end_point1 = *queue.top();
#ifndef NDEBUG
			// Each edge added shall be longer than the previous one taken.
			assert(end_point1.distance_out > distance_taken_last - SCALED_EPSILON);
			distance_taken_last = end_point1.distance_out;
#endif /* NDEBUG */
			assert(end_point1.edge_out != nullptr);
	    	// No point on the queue may be connected yet.
	    	assert(end_point1.chain_id == 0);
	    	// Take the closest end point to the first end point,
	    	EndPoint &end_point2 = *end_point1.edge_out;
	    	bool valid = true;
	    	size_t end_point1_other_chain_id = 0;
	    	size_t end_point2_other_chain_id = 0;
	    	if (end_point2.chain_id > 0) {
	    		// The other side is part of the output path. Don't connect to end_point2, update end_point1 and try another one.
	    		valid = false;
	    	} else {
				// End points of the opposite ends of the segments.
				end_point1_other_chain_id = equivalent_chain(end_points[(&end_point1 - &end_points.front()) ^ 1].chain_id);
				end_point2_other_chain_id = equivalent_chain(end_points[(&end_point2 - &end_points.front()) ^ 1].chain_id);
				if (end_point1_other_chain_id == end_point2_other_chain_id && end_point1_other_chain_id != 0)
					// This edge forms a loop. Update end_point1 and try another one.
					valid = false;
	    	}
	    	if (valid) {
		    	// Remove the first and second point from the queue.
				queue.pop();
		    	queue.remove(end_point2.heap_idx);
		    	assert(end_point1.edge_out = &end_point2);
		    	end_point2.edge_out = &end_point1;
		    	end_point2.distance_out = end_point1.distance_out;
		    	// Assign chain IDs to the newly connected end points, set equivalent_chain if two chains were merged.
		    	size_t chain_id =
					(end_point1_other_chain_id == 0) ?
						((end_point2_other_chain_id == 0) ? equivalent_chain.next() : end_point2_other_chain_id) :
						((end_point2_other_chain_id == 0) ? end_point1_other_chain_id : 
							(end_point1_other_chain_id == end_point2_other_chain_id) ? 
								end_point1_other_chain_id :
								equivalent_chain.merge(end_point1_other_chain_id, end_point2_other_chain_id));
				end_point1.chain_id = chain_id;
				end_point2.chain_id = chain_id;
				assert(validate_graph_and_queue());
				if (iter == 0) {
					// Last iteration. There shall be exactly one or two end points waiting to be connected.
					assert(queue.size() == ((first_point == nullptr) ? 2 : 1));
					if (first_point == nullptr) {
						first_point = queue.top();
						queue.pop();
						first_point->edge_out = nullptr;
					}
					last_point = queue.top();
					last_point->edge_out = nullptr;
					queue.pop();
					assert(queue.empty());
					break;
				}
	    	} else {
				// This edge forms a loop. Update end_point1 and try another one.
				++ iter;
				end_point1.edge_out = nullptr;
		    	// Update edge_out and distance.
		    	size_t this_idx = &end_point1 - &end_points.front();
		    	// Find the closest point to this end_point, which lies on a different extrusion path (filtered by the filter lambda).
				size_t next_idx = find_closest_point(kdtree, end_point1.pos, [&end_points, &equivalent_chain, this_idx](size_t idx) { 
			    	assert(end_points[this_idx].edge_out == nullptr);
			    	assert(end_points[this_idx].chain_id == 0);
					if ((idx ^ this_idx) <= 1 || end_points[idx].chain_id != 0)
						// Points of the same segment shall not be connected,
						// cannot connect to an already connected point (ideally those would be removed from the KD tree, but the update is difficult).
						return false;
			    	size_t chain1 = equivalent_chain(end_points[this_idx ^ 1].chain_id);
			    	size_t chain2 = equivalent_chain(end_points[idx      ^ 1].chain_id);
			    	return chain1 != chain2 || chain1 == 0;
				});
				assert(next_idx < end_points.size());
				end_point1.edge_out = &end_points[next_idx];
				end_point1.distance_out = (end_points[next_idx].pos - end_point1.pos).squaredNorm();
#ifndef NDEBUG
				// Each edge shall be longer than the last one removed from the queue.
				assert(end_point1.distance_out > distance_taken_last - SCALED_EPSILON);
#endif /* NDEBUG */
				// Update position of this end point in the queue based on the distance calculated at the line above.
				queue.update(end_point1.heap_idx);
		    	//FIXME Remove the other end point from the KD tree.
		    	// As the KD tree update is expensive, do it only after some larger number of points is removed from the queue.
				assert(validate_graph_and_queue());
	    	}
		}
		assert(queue.empty());

		// Now interconnect pairs of segments into a chain.
		assert(first_point != nullptr);
		out.reserve(num_segments);
		bool      failed = false;
		do {
			assert(out.size() < num_segments);
			size_t    		 first_point_id = first_point - &end_points.front();
			size_t           segment_id 	= first_point_id >> 1;
			bool             reverse        = (first_point_id & 1) != 0;
			EndPoint 		*second_point   = &end_points[first_point_id ^ 1];
			if (REVERSE_COULD_FAIL) {
				if (reverse && ! could_reverse_func(segment_id)) {
					failed = true;
					break;
				}
			} else {
				assert(! reverse || could_reverse_func(segment_id));
			}
			out.emplace_back(segment_id, reverse);
			first_point = second_point->edge_out;
		} while (first_point != nullptr);
		if (REVERSE_COULD_FAIL) {
			if (failed) {
				if (start_near == nullptr) {
					// We may try the reverse order.
					out.clear();
					first_point = last_point;
					failed = false;
					do {
						assert(out.size() < num_segments);
						size_t    		 first_point_id = first_point - &end_points.front();
						size_t           segment_id 	= first_point_id >> 1;
						bool             reverse        = (first_point_id & 1) != 0;
						EndPoint 		*second_point   = &end_points[first_point_id ^ 1];
						if (reverse && ! could_reverse_func(segment_id)) {
							failed = true;
							break;
						}
						out.emplace_back(segment_id, reverse);
						first_point = second_point->edge_out;
					} while (first_point != nullptr);
				}
			}
			if (failed)
				// As a last resort, try a dumb algorithm, which is not sensitive to edge reversal constraints.
				out = chain_segments_closest_point<EndPoint, decltype(kdtree), CouldReverseFunc>(end_points, kdtree, could_reverse_func, (initial_point != nullptr) ? *initial_point : end_points.front());
		} else {
			assert(! failed);
		}
	}

	assert(out.size() == num_segments);
	return out;
}

template<typename QueueType, typename KDTreeType, typename ChainsType, typename EndPointType>
void update_end_point_in_queue(QueueType &queue, const KDTreeType &kdtree, ChainsType &chains, std::vector<EndPointType> &end_points, EndPointType &end_point, size_t first_point_idx, const EndPointType *first_point)
{
	// Updating an end point or a 2nd from an end point.
	size_t this_idx = end_point.index(end_points);
	// If this segment is not the starting segment, then this end point or the opposite is unconnected.
	assert(first_point_idx == this_idx || first_point_idx == (this_idx ^ 1) || end_point.chain_id == 0 || end_point.opposite(end_points).chain_id == 0);
	end_point.edge_candidate = nullptr;
	if (first_point_idx == this_idx || (end_point.chain_id > 0 && first_point_idx == (this_idx ^ 1)))
	{
		// One may never flip the 1st edge, don't try it again.
		if (! end_point.heap_idx_invalid())
			queue.remove(end_point.heap_idx);
	}
	else
	{
		// Update edge_candidate and distance.
		size_t chain1a    = end_point.chain_id;
		size_t chain1b    = end_points[this_idx ^ 1].chain_id;
		size_t this_chain = chains.equivalent(std::max(chain1a, chain1b));
		// Find the closest point to this end_point, which lies on a different extrusion path (filtered by the filter lambda).
		size_t next_idx = find_closest_point(kdtree, end_point.pos, [&end_points, &chains, this_idx, first_point_idx, first_point, this_chain](size_t idx) {
	    	assert(end_points[this_idx].edge_candidate == nullptr);
	    	// Either this end of the edge or the other end of the edge is not yet connected.
	    	assert((end_points[this_idx    ].chain_id == 0 && end_points[this_idx    ].edge_out == nullptr) ||
	    		   (end_points[this_idx ^ 1].chain_id == 0 && end_points[this_idx ^ 1].edge_out == nullptr));
			if ((idx ^ this_idx) <= 1 || idx == first_point_idx)
				// Points of the same segment shall not be connected.
				// Don't connect to the first point, we must not flip the 1st edge.
				return false;
			size_t chain2a = end_points[idx].chain_id;
			size_t chain2b = end_points[idx ^ 1].chain_id;
			if (chain2a > 0 && chain2b > 0)
				// Only unconnected end point or a point next to an unconnected end point may be connected to.
				// Ideally those would be removed from the KD tree, but the update is difficult.
				return false;
	    	assert(chain2a == 0 || chain2b == 0);
	    	size_t chain2 = chains.equivalent(std::max(chain2a, chain2b));
	    	if (this_chain == chain2)
	    		// Don't connect back to the same chain, don't create a loop.
	    		return this_chain == 0;
	    	// Don't connect to a segment requiring flipping if the segment starts or ends with the first point.
			if (chain2a > 0) {
				// Chain requires flipping.
				assert(chain2b == 0);
				auto &chain = chains.chain(chain2);
				if (chain.begin == first_point || chain.end == first_point)
					return false;
			}
			// Everything is all right, try to connect.
			return true;
		});
		assert(next_idx < end_points.size());
		assert(chains.equivalent(end_points[next_idx].chain_id) != chains.equivalent(end_points[next_idx ^ 1].chain_id) || end_points[next_idx].chain_id == 0);
		end_point.edge_candidate = &end_points[next_idx];
		end_point.distance_out   = (end_points[next_idx].pos - end_point.pos).norm();
		if (end_point.chain_id > 0)
			end_point.distance_out += chains.chain_flip_penalty(this_chain);
		if (end_points[next_idx].chain_id > 0)
			// The candidate chain is flipped.
			end_point.distance_out += chains.chain_flip_penalty(end_points[next_idx].chain_id);
		// Update position of this end point in the queue based on the distance calculated at the line above.
		if (end_point.heap_idx_invalid())
			queue.push(&end_point);
		else
			queue.update(end_point.heap_idx);
	}
}

template<typename PointType, typename SegmentEndPointFunc, bool REVERSE_COULD_FAIL, typename CouldReverseFunc>
std::vector<std::pair<size_t, bool>> chain_segments_greedy_constrained_reversals2_(SegmentEndPointFunc end_point_func, CouldReverseFunc could_reverse_func, size_t num_segments, const PointType *start_near)
{
	std::vector<std::pair<size_t, bool>> out;

	if (num_segments == 0) {
		// Nothing to do.
	} 
	else if (num_segments == 1)
	{
		// Just sort the end points so that the first point visited is closest to start_near.
		out.emplace_back(0, start_near != nullptr && 
            (end_point_func(0, true) - *start_near).template cast<double>().squaredNorm() < (end_point_func(0, false) - *start_near).template cast<double>().squaredNorm());
	} 
	else
	{
		// End points of segments for the KD tree closest point search.
		// A single end point is inserted into the search structure for loops, two end points are entered for open paths.
		struct EndPoint {
			EndPoint(const Vec2d &pos) : pos(pos) {}
			Vec2d     pos;

			// Candidate for a new connection link.
			EndPoint *edge_candidate = nullptr;
			// Distance to the next end point following the link.
			// Zero value -> start of the final path.
			double    distance_out = std::numeric_limits<double>::max();

			size_t    heap_idx = std::numeric_limits<size_t>::max();
			bool 	  heap_idx_invalid() const { return this->heap_idx == std::numeric_limits<size_t>::max(); }

			// Identifier of the chain, to which this end point belongs. Zero means unassigned.
			size_t    chain_id = 0;
			// Double linked chain of segment end points in current path.
			EndPoint *edge_out = nullptr;

			size_t 				index(std::vector<EndPoint>          &endpoints) const { return this - endpoints.data(); }
			// Opposite end point of the same segment.
			EndPoint& 			opposite(std::vector<EndPoint>       &endpoints)       { return endpoints[(this - endpoints.data()) ^ 1]; }
			const EndPoint& 	opposite(const std::vector<EndPoint> &endpoints) const { return endpoints[(this - endpoints.data()) ^ 1]; }
		};

	    std::vector<EndPoint> end_points;
	    end_points.reserve(num_segments * 2);
	    for (size_t i = 0; i < num_segments; ++ i) {
            end_points.emplace_back(end_point_func(i, true ).template cast<double>());
            end_points.emplace_back(end_point_func(i, false).template cast<double>());
	    }

	    // Construct the closest point KD tree over end points of segments.
		auto coordinate_fn = [&end_points](size_t idx, size_t dimension) -> double { return end_points[idx].pos[dimension]; };
		KDTreeIndirect<2, double, decltype(coordinate_fn)> kdtree(coordinate_fn, end_points.size());

	    // Chained segments with their sum of connection lengths.
	    // The chain supports flipping all the segments, connecting the segments at the opposite ends.
	    // (this is a very useful path optimization for infill lines).
	    struct Chain {
	    	size_t		 num_segments	 = 0;
	    	double 		 cost 			 = 0.;
	    	double 		 cost_flipped	 = 0.;
	    	EndPoint 	*begin 			 = nullptr;
	    	EndPoint 	*end 			 = nullptr;
	    	size_t  	 equivalent_with = 0;

	    	// Flipping the chain has a time complexity of O(n).
	    	void flip(std::vector<EndPoint> &endpoints)
	    	{
				assert(this->num_segments > 1);
				assert(this->begin->edge_out == nullptr);
	    		assert(this->end  ->edge_out == nullptr);
				assert(this->begin->opposite(endpoints).edge_out != nullptr);
				assert(this->end  ->opposite(endpoints).edge_out != nullptr);
				// Start of the current segment processed.
	    		EndPoint *ept      = this->begin;
	    		// Previous end point to connect the other side of ept to.
	    		EndPoint *ept_prev = nullptr;
	    		do {
	    			EndPoint *ept_end      = &ept->opposite(endpoints);
	    			EndPoint *ept_next     = ept_end->edge_out;
	    			assert(ept_next == nullptr || ept_next->edge_out == ept_end);
	    			// Connect to the preceding segment.
	    			ept_end->edge_out = ept_prev;
	    			if (ept_prev != nullptr)
	    				ept_prev->edge_out = ept_end;
	    			ept_prev = ept;
	    			ept = ept_next;
	    		} while (ept != nullptr);
	    		ept_prev->edge_out = nullptr;
	    		// Swap the costs.
	    		std::swap(this->cost, this->cost_flipped);
				// Swap the ends.
				EndPoint *new_begin = &this->begin->opposite(endpoints);
				EndPoint *new_end   = &this->end->opposite(endpoints);
				std::swap(this->begin->chain_id, new_begin->chain_id);
				std::swap(this->end  ->chain_id, new_end  ->chain_id);
				this->begin = new_begin;
				this->end   = new_end;
				assert(this->begin->edge_out == nullptr);
	    		assert(this->end  ->edge_out == nullptr);
				assert(this->begin->opposite(endpoints).edge_out != nullptr);
				assert(this->end  ->opposite(endpoints).edge_out != nullptr);
			}

	    	double flip_penalty() const { return this->cost_flipped - this->cost; }
	    };

		// Helper to detect loops in already connected paths and to accomodate flipping of chains.
		//	
		// Unique chain IDs are assigned to paths. If paths are connected, end points will not have their chain IDs updated, but the chain IDs
		// will remember an "equivalent" chain ID, which is the lowest ID of all the IDs in the path, and the lowest ID is equivalent to itself.
		// Chain IDs are indexed starting with 1.
		// 
		// Chains remember their lengths and their lengths when each segment of the chain is flipped.
		class Chains {
		public:
			// Zero'th chain ID is invalid.
			Chains(size_t reserve) { 
				m_chains.reserve(reserve / 2);
				// Indexing starts with 1.
				m_chains.emplace_back();
			}

			// Generate next equivalence class.
			size_t 				next_id() {
				m_chains.emplace_back();
				m_chains.back().equivalent_with = ++ m_last_chain_id;
				return m_last_chain_id;
			}

			// Get equivalence class for chain ID, update the "equivalent_with" along the equivalence path.
			size_t 				equivalent(size_t chain_id) {
				if (chain_id != 0) {
					for (size_t last = chain_id;;) {
						size_t lower = m_chains[last].equivalent_with;
						if (lower == last) {
							m_chains[chain_id].equivalent_with = lower;
							chain_id = lower;
							break;
						}
						last = lower;
					}
				}
				return chain_id;
			}

			// Return a lowest chain ID of the two input chains.
			// Produce a new chain ID of both chain IDs are zero.
			size_t 			  	merge(size_t chain_id1, size_t chain_id2) {
				if (chain_id1 == 0)
					return (chain_id2 == 0) ? this->next_id() : chain_id2;
				if (chain_id2 == 0)
					return chain_id1;
				assert(m_chains[chain_id1].equivalent_with == chain_id1);
				assert(m_chains[chain_id2].equivalent_with == chain_id2);
				size_t chain_id = std::min(chain_id1, chain_id2);
				m_chains[chain_id1].equivalent_with = chain_id;
				m_chains[chain_id2].equivalent_with = chain_id;
				return chain_id;
			}

			Chain& 				chain(size_t chain_id) 		 { return m_chains[chain_id]; }
			const Chain&		chain(size_t chain_id) const { return m_chains[chain_id]; }

			double 				chain_flip_penalty(size_t chain_id) {
				chain_id = this->equivalent(chain_id);
				return m_chains[chain_id].flip_penalty();
			}

#ifndef NDEBUG
			bool				validate()
			{
				// Validate that the segments merged chain IDs make up a directed acyclic graph
				// with edges oriented towards the lower chain ID, therefore all ending up
				// in the lowest chain ID of all of them.
				assert(m_last_chain_id >= 0);
				assert(m_last_chain_id + 1 == m_chains.size());
				for (size_t i = 0; i < m_chains.size(); ++ i) {
					for (size_t last = i;;) {
						size_t lower = m_chains[last].equivalent_with;
						assert(lower <= last);
						if (lower == last)
							break;
						last = lower;
					}
				}
				return true;
			}
#endif /* NDEBUG */

		private:
			std::vector<Chain> 	m_chains;
			// Unique chain ID assigned to chains of end points of segments.
			size_t              m_last_chain_id = 0;
		} chains(num_segments);

		// Find the first end point closest to start_near.
		EndPoint *first_point = nullptr;
		size_t    first_point_idx = std::numeric_limits<size_t>::max();
		if (start_near != nullptr) {
            size_t idx = find_closest_point(kdtree, start_near->template cast<double>());
			assert(idx < end_points.size());
			first_point = &end_points[idx];
			first_point->distance_out = 0.;
			first_point->chain_id = chains.next_id();
			Chain &chain = chains.chain(first_point->chain_id);
			chain.begin = first_point;
			chain.end   = &first_point->opposite(end_points);
			first_point_idx = idx;
		}
		EndPoint *initial_point = first_point;
		EndPoint *last_point = nullptr;

		// Assign the closest point and distance to the end points.
		for (EndPoint &end_point : end_points) {
	    	assert(end_point.edge_candidate == nullptr);
	    	if (&end_point != first_point) {
		    	size_t this_idx = end_point.index(end_points);
		    	// Find the closest point to this end_point, which lies on a different extrusion path (filtered by the lambda).
		    	// Ignore the starting point as the starting point is considered to be occupied, no end point coud connect to it.
				size_t next_idx = find_closest_point(kdtree, end_point.pos, 
					[this_idx, first_point_idx](size_t idx){ return idx != first_point_idx && (idx ^ this_idx) > 1; });
				assert(next_idx < end_points.size());
				EndPoint &end_point2 = end_points[next_idx];
				end_point.edge_candidate = &end_point2;
				end_point.distance_out = (end_point2.pos - end_point.pos).norm();
			}
		}

	    // Initialize a heap of end points sorted by the lowest distance to the next valid point of a path.
	    auto queue = make_mutable_priority_queue<EndPoint*, true>(
			[](EndPoint *ep, size_t idx){ ep->heap_idx = idx; }, 
	    	[](EndPoint *l, EndPoint *r){ return l->distance_out < r->distance_out; });
		queue.reserve(end_points.size() * 2);
	    for (EndPoint &ep : end_points)
	    	if (first_point != &ep)
				queue.push(&ep);

#ifndef NDEBUG
		auto validate_graph_and_queue = [&chains, &end_points, &queue, first_point]() -> bool {
			assert(chains.validate());
			for (EndPoint &ep : end_points) {
				if (ep.heap_idx < queue.size()) {
					// End point is on the heap.
					assert(*(queue.cbegin() + ep.heap_idx) == &ep);
					// One side or the other of the segment is not yet connected.
					assert(ep.chain_id == 0 || ep.opposite(end_points).chain_id == 0);
				} else {
					// End point is NOT on the heap, therefore it must part of the output path.
					assert(ep.heap_idx_invalid());
					assert(ep.chain_id != 0);
					if (&ep == first_point) {
						assert(ep.edge_out == nullptr);
					} else {
						assert(ep.edge_out != nullptr);
						// Detect loops.
						for (EndPoint *pt = &ep; pt != nullptr;) {
							// Out of queue. It is a final point.
							EndPoint *pt_other = &pt->opposite(end_points);
							if (pt_other->heap_idx < queue.size()) {
								// The other side of this segment is undecided yet.
								// assert(pt_other->edge_out == nullptr);
								break;
							}
							pt = pt_other->edge_out;
						}
					}
				}
			}
			for (EndPoint *ep : queue)
				// Points in the queue or the opposites of the same segment are not connected yet.
				assert(ep->chain_id == 0 || ep->opposite(end_points).chain_id == 0);
			return true;
		};
#endif /* NDEBUG */

	    // Chain the end points: find (num_segments - 1) shortest links not forming bifurcations or loops.
		assert(num_segments >= 2);
#ifndef NDEBUG
		double distance_taken_last = 0.;
#endif /* NDEBUG */
		// Some links stored onto the priority queue are being invalidated during the calculation and they are not
		// updated immediately. If such a situation is detected for an end point pulled from the priority queue,
		// the end point is being updated and re-inserted into the priority queue. Therefore the number of iterations
		// required is higher than expected (it would be the number of links, num_segments - 1).
		// The limit here may not be necessary, but it guards us against an endless loop if something goes wrong.
		size_t num_iter = num_segments * 16;
		for (size_t num_connections_to_end = num_segments - 1; num_iter > 0; -- num_iter) {
			assert(validate_graph_and_queue());
	    	// Take the first end point, for which the link points to the currently closest valid neighbor.
	    	EndPoint *end_point1       = queue.top();
	    	assert(end_point1 != first_point);
	    	EndPoint *end_point1_other = &end_point1->opposite(end_points);
	    	// true if end_point1 is not the end of its chain, but the 2nd point. When connecting to the 2nd point, this chain needs
	    	// to be flipped first.
	    	bool      chain1_flip      = end_point1->chain_id > 0;
	    	// Either this point at the queue is not connected, or it is the 2nd point of a chain.
	    	// If connecting to a 2nd point of a chain, the 1st point shall not yet be connected and this chain will need
	    	// to be flipped.
	    	assert(  chain1_flip || (end_point1->chain_id == 0 && end_point1->edge_out == nullptr));
	    	assert(! chain1_flip || (end_point1_other->chain_id == 0 && end_point1_other->edge_out == nullptr));
			assert(end_point1->edge_candidate != nullptr);
#ifndef NDEBUG 
			// Each edge added shall be longer than the previous one taken.
			//assert(end_point1->distance_out > distance_taken_last - SCALED_EPSILON);
			if (end_point1->distance_out < distance_taken_last - SCALED_EPSILON) {
//				printf("Warning: taking shorter length than previously is suspicious\n");
			}
			distance_taken_last = end_point1->distance_out;
#endif /* NDEBUG */
	    	// Take the closest end point to the first end point,
	    	EndPoint *end_point2 	   = end_point1->edge_candidate;
	    	EndPoint *end_point2_other = &end_point2->opposite(end_points);
	    	bool      chain2_flip      = end_point2->chain_id > 0;
	    	// Is the link from end_point1 to end_point2 still valid? If yes, the link may be taken. Otherwise the link needs to be refreshed.
	    	bool      valid            = true;
	    	size_t 	  end_point1_chain_id = 0;
	    	size_t    end_point2_chain_id = 0;
	    	if (end_point2->chain_id > 0 && end_point2_other->chain_id > 0) {
	    		// The other side is part of the output path. Don't connect to end_point2, update end_point1 and try another one.
	    		valid = false;
	    	} else {
				// End points of the opposite ends of the segments.
				end_point1_chain_id = chains.equivalent((chain1_flip ? end_point1 : end_point1_other)->chain_id);
				end_point2_chain_id = chains.equivalent((chain2_flip ? end_point2 : end_point2_other)->chain_id);
				if (end_point1_chain_id == end_point2_chain_id && end_point1_chain_id != 0)
					// This edge forms a loop. Update end_point1 and try another one.
					valid = false;
				else {
					// Verify whether end_point1.distance_out still matches the current state of the two end points to be connected and their chains.
					// Namely, the other chain may have been flipped in the meantime.
					double dist = (end_point2->pos - end_point1->pos).norm();
					if (chain1_flip)
						dist += chains.chain_flip_penalty(end_point1_chain_id);
					if (chain2_flip)
						dist += chains.chain_flip_penalty(end_point2_chain_id);
					if (std::abs(dist - end_point1->distance_out) > SCALED_EPSILON)
						// The distance changed due to flipping of one of the chains. Refresh this end point in the queue.
						valid = false;
				}
				if (valid && first_point != nullptr) {
					// Verify that a chain starting or ending with the first_point does not get flipped.
					if (chain1_flip) {
						Chain &chain = chains.chain(end_point1_chain_id);
						if (chain.begin == first_point || chain.end == first_point)
							valid = false;
					}
					if (valid && chain2_flip) {
						Chain &chain = chains.chain(end_point2_chain_id);
						if (chain.begin == first_point || chain.end == first_point)
							valid = false;
					}
				}
	    	}
	    	if (valid) {
		    	// Remove the first and second point from the queue.
				queue.pop();
		    	queue.remove(end_point2->heap_idx);
		    	assert(end_point1->edge_candidate == end_point2);
		    	end_point1->edge_candidate = nullptr;
		    	Chain *chain1 = (end_point1_chain_id == 0) ? nullptr : &chains.chain(end_point1_chain_id);
		    	Chain *chain2 = (end_point2_chain_id == 0) ? nullptr : &chains.chain(end_point2_chain_id);
				assert(chain1 == nullptr || (chain1_flip ? (chain1->begin == end_point1_other || chain1->end == end_point1_other) : (chain1->begin == end_point1 || chain1->end == end_point1)));
				assert(chain2 == nullptr || (chain2_flip ? (chain2->begin == end_point2_other || chain2->end == end_point2_other) : (chain2->begin == end_point2 || chain2->end == end_point2)));
				if (chain1_flip)
		    		chain1->flip(end_points);
		    	if (chain2_flip)
		    		chain2->flip(end_points);
				assert(chain1 == nullptr || chain1->begin == end_point1 || chain1->end == end_point1);
				assert(chain2 == nullptr || chain2->begin == end_point2 || chain2->end == end_point2);
		    	size_t chain_id = chains.merge(end_point1_chain_id, end_point2_chain_id);
				Chain &chain    = chains.chain(chain_id);
				{
				    Chain chain_dst;
					chain_dst.begin = (chain1 == nullptr) ? end_point1_other : (chain1->begin == end_point1) ? chain1->end : chain1->begin;
			    	chain_dst.end   = (chain2 == nullptr) ? end_point2_other : (chain2->begin == end_point2) ? chain2->end : chain2->begin;
			    	chain_dst.cost  = (chain1 == 0 ? 0. : chain1->cost) + (chain2 == 0 ? 0. : chain2->cost) + (end_point2->pos - end_point1->pos).norm();
			    	chain_dst.cost_flipped = (chain1 == 0 ? 0. : chain1->cost_flipped) + (chain2 == 0 ? 0. : chain2->cost_flipped) + (end_point2_other->pos - end_point1_other->pos).norm();
			    	chain_dst.num_segments = (chain1 == 0 ? 1 : chain1->num_segments) + (chain2 == 0 ? 1 : chain2->num_segments);
				    chain_dst.equivalent_with = chain_id;
					chain = chain_dst;
			    }
				if (chain.begin != end_point1_other && ! end_point1_other->heap_idx_invalid())
					queue.remove(end_point1_other->heap_idx);
				if (chain.end != end_point2_other && ! end_point2_other->heap_idx_invalid())
					queue.remove(end_point2_other->heap_idx);
				end_point1->edge_out = end_point2;
				end_point2->edge_out = end_point1;
				end_point1->chain_id = chain_id;
				end_point2->chain_id = chain_id;
				end_point1_other->chain_id = chain_id;
				end_point2_other->chain_id = chain_id;
				if (chain.begin != first_point)
					chain.begin->chain_id = 0;
				if (chain.end != first_point)
					chain.end->chain_id = 0;
				if (-- num_connections_to_end == 0) {
					assert(validate_graph_and_queue());
					// Last iteration. There shall be exactly one or two end points waiting to be connected.
					assert(queue.size() <= ((first_point == nullptr) ? 4 : 2));
					if (first_point == nullptr) {
						// Find the first remaining end point.
						do {
							first_point = queue.top();
							queue.pop();
						} while (first_point->edge_out != nullptr);
						assert(first_point->edge_out == nullptr);
					}
					// Find the first remaining end point.
					do {
						last_point = queue.top();
						queue.pop();
					} while (last_point->edge_out != nullptr);
					assert(last_point->edge_out == nullptr);
#ifndef NDEBUG
					while (! queue.empty()) {
						assert(queue.top()->edge_out != nullptr && queue.top()->chain_id > 0);
						queue.pop();
					}
#endif /* NDEBUG */
					break;
				} else {
					//FIXME update the 2nd end points on the queue.
					// Update end points of the flipped segments.
					update_end_point_in_queue(queue, kdtree, chains, end_points, chain.begin->opposite(end_points), first_point_idx, first_point);
					update_end_point_in_queue(queue, kdtree, chains, end_points, chain.end->opposite(end_points),   first_point_idx, first_point);
					if (chain1_flip)
						update_end_point_in_queue(queue, kdtree, chains, end_points, *chain.begin, first_point_idx, first_point);
					if (chain2_flip)
						update_end_point_in_queue(queue, kdtree, chains, end_points, *chain.end,   first_point_idx, first_point);
					// End points of chains shall certainly stay in the queue.
					assert(chain.begin == first_point || chain.begin->heap_idx < queue.size());
					assert(chain.end   == first_point || chain.end  ->heap_idx < queue.size());
					assert(&chain.begin->opposite(end_points) != first_point &&
						(chain.begin == first_point ? chain.begin->opposite(end_points).heap_idx_invalid() : chain.begin->opposite(end_points).heap_idx < queue.size()));
					assert(&chain.end  ->opposite(end_points) != first_point &&
						(chain.end   == first_point ? chain.end  ->opposite(end_points).heap_idx_invalid() : chain.end  ->opposite(end_points).heap_idx < queue.size()));

				}
			} else {
				// This edge forms a loop. Update end_point1 and try another one.
				update_end_point_in_queue(queue, kdtree, chains, end_points, *end_point1, first_point_idx, first_point);
#ifndef NDEBUG
				// Each edge shall be longer than the last one removed from the queue.
				//assert(end_point1->distance_out > distance_taken_last - SCALED_EPSILON);
				if (end_point1->distance_out < distance_taken_last - SCALED_EPSILON) {
//					printf("Warning: taking shorter length than previously is suspicious\n");
				}
#endif /* NDEBUG */
		    	//FIXME Remove the other end point from the KD tree.
		    	// As the KD tree update is expensive, do it only after some larger number of points is removed from the queue.
		    }
			assert(validate_graph_and_queue());
		}
		assert(queue.empty());

		// Now interconnect pairs of segments into a chain.
		assert(first_point != nullptr);
		out.reserve(num_segments);
		bool      failed = false;
		do {
			assert(out.size() < num_segments);
			size_t    		 first_point_id = first_point - &end_points.front();
			size_t           segment_id 	= first_point_id >> 1;
			bool             reverse        = (first_point_id & 1) != 0;
			EndPoint 		*second_point   = &end_points[first_point_id ^ 1];
			if (REVERSE_COULD_FAIL) {
				if (reverse && ! could_reverse_func(segment_id)) {
					failed = true;
					break;
				}
			} else {
				assert(! reverse || could_reverse_func(segment_id));
			}
			out.emplace_back(segment_id, reverse);
			first_point = second_point->edge_out;
		} while (first_point != nullptr);
		if (REVERSE_COULD_FAIL) {
			if (failed) {
				if (start_near == nullptr) {
					// We may try the reverse order.
					out.clear();
					first_point = last_point;
					failed = false;
					do {
						assert(out.size() < num_segments);
						size_t    		 first_point_id = first_point - &end_points.front();
						size_t           segment_id 	= first_point_id >> 1;
						bool             reverse        = (first_point_id & 1) != 0;
						EndPoint 		*second_point   = &end_points[first_point_id ^ 1];
						if (reverse && ! could_reverse_func(segment_id)) {
							failed = true;
							break;
						}
						out.emplace_back(segment_id, reverse);
						first_point = second_point->edge_out;
					} while (first_point != nullptr);
				}
			}
			if (failed)
				// As a last resort, try a dumb algorithm, which is not sensitive to edge reversal constraints.
				out = chain_segments_closest_point<EndPoint, decltype(kdtree), CouldReverseFunc>(end_points, kdtree, could_reverse_func, (initial_point != nullptr) ? *initial_point : end_points.front());
		} else {
			assert(! failed);
		}
	}

	assert(out.size() == num_segments);
	return out;
}

template<typename PointType, typename SegmentEndPointFunc, typename CouldReverseFunc>
std::vector<std::pair<size_t, bool>> chain_segments_greedy_constrained_reversals(SegmentEndPointFunc end_point_func, CouldReverseFunc could_reverse_func, size_t num_segments, const PointType *start_near)
{
	return chain_segments_greedy_constrained_reversals_<PointType, SegmentEndPointFunc, true, CouldReverseFunc>(end_point_func, could_reverse_func, num_segments, start_near);
}

template<typename PointType, typename SegmentEndPointFunc>
std::vector<std::pair<size_t, bool>> chain_segments_greedy(SegmentEndPointFunc end_point_func, size_t num_segments, const PointType *start_near)
{
	auto could_reverse_func = [](size_t /* idx */) -> bool { return true; };
	return chain_segments_greedy_constrained_reversals_<PointType, SegmentEndPointFunc, false, decltype(could_reverse_func)>(end_point_func, could_reverse_func, num_segments, start_near);
}

template<typename PointType, typename SegmentEndPointFunc, typename CouldReverseFunc>
std::vector<std::pair<size_t, bool>> chain_segments_greedy_constrained_reversals2(SegmentEndPointFunc end_point_func, CouldReverseFunc could_reverse_func, size_t num_segments, const PointType *start_near)
{
	return chain_segments_greedy_constrained_reversals2_<PointType, SegmentEndPointFunc, true, CouldReverseFunc>(end_point_func, could_reverse_func, num_segments, start_near);
}

template<typename PointType, typename SegmentEndPointFunc>
std::vector<std::pair<size_t, bool>> chain_segments_greedy2(SegmentEndPointFunc end_point_func, size_t num_segments, const PointType *start_near)
{
	auto could_reverse_func = [](size_t /* idx */) -> bool { return true; };
	return chain_segments_greedy_constrained_reversals2_<PointType, SegmentEndPointFunc, false, decltype(could_reverse_func)>(end_point_func, could_reverse_func, num_segments, start_near);
}

std::vector<std::pair<size_t, bool>> chain_extrusion_entities(std::vector<ExtrusionEntity*> &entities, const Point *start_near)
{
	auto segment_end_point = [&entities](size_t idx, bool first_point) -> const Point& { return first_point ? entities[idx]->first_point() : entities[idx]->last_point(); };
	auto could_reverse = [&entities](size_t idx) { const ExtrusionEntity *ee = entities[idx]; return ee->is_loop() || ee->can_reverse(); };
	std::vector<std::pair<size_t, bool>> out = chain_segments_greedy_constrained_reversals<Point, decltype(segment_end_point), decltype(could_reverse)>(segment_end_point, could_reverse, entities.size(), start_near);
	for (std::pair<size_t, bool> &segment : out) {
		ExtrusionEntity *ee = entities[segment.first];
		if (ee->is_loop())
			// Ignore reversals for loops, as the start point equals the end point.
			segment.second = false;
		// Is can_reverse() respected by the reversals?
		assert(ee->can_reverse() || ! segment.second);
	}
	return out;
}

void reorder_extrusion_entities(std::vector<ExtrusionEntity*> &entities, const std::vector<std::pair<size_t, bool>> &chain)
{
	assert(entities.size() == chain.size());
	std::vector<ExtrusionEntity*> out;
	out.reserve(entities.size());
    for (const std::pair<size_t, bool> &idx : chain) {
		assert(entities[idx.first] != nullptr);
        out.emplace_back(entities[idx.first]);
        if (idx.second)
			out.back()->reverse();
    }
    entities.swap(out);
}

void chain_and_reorder_extrusion_entities(std::vector<ExtrusionEntity*> &entities, const Point *start_near)
{
    // this function crashes if there are empty elements in entities
    entities.erase(std::remove_if(entities.begin(), entities.end(), [](ExtrusionEntity *entity) { return static_cast<ExtrusionEntityCollection *>(entity)->empty(); }),
                   entities.end());
	reorder_extrusion_entities(entities, chain_extrusion_entities(entities, start_near));
}

std::vector<std::pair<size_t, bool>> chain_extrusion_paths(std::vector<ExtrusionPath> &extrusion_paths, const Point *start_near)
{
	auto segment_end_point = [&extrusion_paths](size_t idx, bool first_point) -> const Point& { return first_point ? extrusion_paths[idx].first_point() : extrusion_paths[idx].last_point(); };
	return chain_segments_greedy<Point, decltype(segment_end_point)>(segment_end_point, extrusion_paths.size(), start_near);
}

void reorder_extrusion_paths(std::vector<ExtrusionPath> &extrusion_paths, const std::vector<std::pair<size_t, bool>> &chain)
{
	assert(extrusion_paths.size() == chain.size());
	if(extrusion_paths.empty()) return;
	std::vector<ExtrusionPath> out;
	out.reserve(extrusion_paths.size());
    for (const std::pair<size_t, bool> &idx : chain) {
        out.emplace_back(std::move(extrusion_paths[idx.first]));
        if (idx.second)
			out.back().reverse();
    }
    extrusion_paths.swap(out);
}

void chain_and_reorder_extrusion_paths(std::vector<ExtrusionPath> &extrusion_paths, const Point *start_near)
{
	reorder_extrusion_paths(extrusion_paths, chain_extrusion_paths(extrusion_paths, start_near));
}

std::vector<size_t> chain_expolygons(const ExPolygons &input_exploy) {
	Points points;
	for (const ExPolygon &exploy : input_exploy) {
		BoundingBox bbox;
		bbox = get_extents(exploy);
		points.push_back(bbox.center());
	}
	return chain_points(points);
}

std::vector<size_t> chain_points(const Points &points, Point *start_near)
{
	auto segment_end_point = [&points](size_t idx, bool /* first_point */) -> const Point& { return points[idx]; };
	std::vector<std::pair<size_t, bool>> ordered = chain_segments_greedy<Point, decltype(segment_end_point)>(segment_end_point, points.size(), start_near);
	std::vector<size_t> out;
	out.reserve(ordered.size());
	for (auto &segment_and_reversal : ordered)
		out.emplace_back(segment_and_reversal.first);
	return out;
}

#ifndef NDEBUG
	// #define DEBUG_SVG_OUTPUT
#endif /* NDEBUG */

#ifdef DEBUG_SVG_OUTPUT
void svg_draw_polyline_chain(const char *name, size_t idx, const Polylines &polylines)
{
	BoundingBox bbox = get_extents(polylines);
	SVG svg(debug_out_path("%s-%d.svg", name, idx).c_str(), bbox);
	svg.draw(polylines);
	for (size_t i = 1; i < polylines.size(); ++i)
		svg.draw(Line(polylines[i - 1].last_point(), polylines[i].first_point()), "red");
}
#endif /* DEBUG_SVG_OUTPUT */

#if 0
// Flip the sequences of polylines to lower the total length of connecting lines.
static inline void improve_ordering_by_segment_flipping(Polylines &polylines, bool fixed_start)
{
#ifndef NDEBUG
	auto cost = [&polylines]() {
		double sum = 0.;
		for (size_t i = 1; i < polylines.size(); ++i)
			sum += (polylines[i].first_point() - polylines[i - 1].last_point()).cast<double>().norm();
		return sum;
	};
	double cost_initial = cost();

	static int iRun = 0;
	++ iRun;
#ifdef DEBUG_SVG_OUTPUT
	svg_draw_polyline_chain("improve_ordering_by_segment_flipping-initial", iRun, polylines);
#endif /* DEBUG_SVG_OUTPUT */
#endif /* NDEBUG */

	struct Connection {
		Connection(size_t heap_idx = std::numeric_limits<size_t>::max(), bool flipped = false) : heap_idx(heap_idx), flipped(flipped) {}
		// Position of this object on MutablePriorityHeap.
		size_t 	heap_idx;
		// Is segment_idx flipped?
		bool   	flipped;

		double 			squaredNorm(const Polylines &polylines, const std::vector<Connection> &connections) const 
			{ return ((this + 1)->start_point(polylines, connections) - this->end_point(polylines, connections)).squaredNorm(); }
		double 			norm(const Polylines &polylines, const std::vector<Connection> &connections) const 
			{ return sqrt(this->squaredNorm(polylines, connections)); }
		double 			squaredNorm(const Polylines &polylines, const std::vector<Connection> &connections, bool try_flip1, bool try_flip2) const 
			{ return ((this + 1)->start_point(polylines, connections, try_flip2) - this->end_point(polylines, connections, try_flip1)).squaredNorm(); }
		double 			norm(const Polylines &polylines, const std::vector<Connection> &connections, bool try_flip1, bool try_flip2) const
			{ return sqrt(this->squaredNorm(polylines, connections, try_flip1, try_flip2)); }
		Vec2d			start_point(const Polylines &polylines, const std::vector<Connection> &connections, bool flip = false) const 
			{ const Polyline &pl = polylines[this - connections.data()]; return ((this->flipped == flip) ? pl.points.front() : pl.points.back()).cast<double>(); }
		Vec2d			end_point(const Polylines &polylines, const std::vector<Connection> &connections, bool flip = false) const 
			{ const Polyline &pl = polylines[this - connections.data()]; return ((this->flipped == flip) ? pl.points.back() : pl.points.front()).cast<double>(); }

		bool 			in_queue() const { return this->heap_idx != std::numeric_limits<size_t>::max(); }
		void 			flip() { this->flipped = ! this->flipped; }
	};
	std::vector<Connection> connections(polylines.size());

#ifndef NDEBUG
	auto cost_flipped = [fixed_start, &polylines, &connections]() {
		assert(! fixed_start || ! connections.front().flipped);
		double sum = 0.;
		for (size_t i = 1; i < polylines.size(); ++ i)
			sum += connections[i - 1].norm(polylines, connections);
		return sum;
	};
	double cost_prev = cost_flipped();
	assert(std::abs(cost_initial - cost_prev) < SCALED_EPSILON);

	auto print_statistics = [&polylines, &connections]() {
#if 0
		for (size_t i = 1; i < polylines.size(); ++ i)
			printf("Connecting %d with %d: Current length %lf flip(%d, %d), left flipped: %lf, right flipped: %lf, both flipped: %lf, \n",
				int(i - 1), int(i),
				unscale<double>(connections[i - 1].norm(polylines, connections)),
				int(connections[i - 1].flipped), int(connections[i].flipped),
				unscale<double>(connections[i - 1].norm(polylines, connections, true, false)),
				unscale<double>(connections[i - 1].norm(polylines, connections, false, true)),
				unscale<double>(connections[i - 1].norm(polylines, connections, true, true)));
#endif
	};
	print_statistics();
#endif /* NDEBUG */

    // Initialize a MutablePriorityHeap of connections between polylines.
    auto queue = make_mutable_priority_queue<Connection*, false>(
		[](Connection *connection, size_t idx){ connection->heap_idx = idx; },
		// Sort by decreasing connection distance.
    	[&polylines, &connections](Connection *l, Connection *r){ return l->squaredNorm(polylines, connections) > r->squaredNorm(polylines, connections); });
	queue.reserve(polylines.size() - 1);
    for (size_t i = 0; i + 1 < polylines.size(); ++ i)
		queue.push(&connections[i]);

	static constexpr size_t itercnt = 100;
	size_t iter = 0;
	for (; ! queue.empty() && iter < itercnt; ++ iter) {
		Connection &connection = *queue.top();
		queue.pop();
		connection.heap_idx = std::numeric_limits<size_t>::max();
		size_t idx_first = &connection - connections.data();
		// Try to flip segments starting with idx_first + 1 to the end.
		// Calculate the last segment to be flipped to improve the total path length.
		double length_current 			= connection.norm(polylines, connections);
		double length_flipped 			= connection.norm(polylines, connections, false, true);
		int    best_idx_forward			= int(idx_first);
		double best_improvement_forward = 0.;
		for (size_t i = idx_first + 1; i + 1 < connections.size(); ++ i) {
			length_current += connections[i].norm(polylines, connections);
			double this_improvement = length_current - length_flipped - connections[i].norm(polylines, connections, true, false);
			length_flipped += connections[i].norm(polylines, connections, true, true);
			if (this_improvement > best_improvement_forward) {
				best_improvement_forward = this_improvement;
				best_idx_forward = int(i);
			}
//			if (length_flipped > 1.5 * length_current)
//				break;
		}
		if (length_current - length_flipped > best_improvement_forward)
			// Best improvement by flipping up to the end.
			best_idx_forward = int(connections.size()) - 1;
		// Try to flip segments starting with idx_first - 1 to the start.
		// Calculate the last segment to be flipped to improve the total path length.
		length_current 					  = connection.norm(polylines, connections);
		length_flipped					  = connection.norm(polylines, connections, true, false);
		int    best_idx_backwards		  = int(idx_first);
		double best_improvement_backwards = 0.;
		for (int i = int(idx_first) - 1; i >= 0; -- i) {
			length_current += connections[i].norm(polylines, connections);
			double this_improvement = length_current - length_flipped - connections[i].norm(polylines, connections, false, true);
			length_flipped += connections[i].norm(polylines, connections, true, true);
			if (this_improvement > best_improvement_backwards) {
				best_improvement_backwards = this_improvement;
				best_idx_backwards = int(i);
			}
//			if (length_flipped > 1.5 * length_current)
//				break;
		}
		if (! fixed_start && length_current - length_flipped > best_improvement_backwards)
			// Best improvement by flipping up to the start including the first polyline.
			best_idx_backwards = -1;
		int update_begin = int(idx_first);
		int update_end   = best_idx_forward;
		if (best_improvement_backwards > 0. && best_improvement_backwards > best_improvement_forward) {
			// Flip the sequence of polylines from idx_first to best_improvement_forward + 1.
			update_begin = best_idx_backwards;
			update_end   = int(idx_first);
		}
		assert(update_begin <= update_end);
		if (update_begin == update_end)
			continue;
		for (int i = update_begin + 1; i <= update_end; ++ i)
			connections[i].flip();

#ifndef NDEBUG
		double cost = cost_flipped();
		assert(cost < cost_prev);
		cost_prev = cost;
		print_statistics();
#endif /* NDEBUG */

		update_end = std::min(update_end + 1, int(connections.size()) - 1);
		for (int i = std::max(0, update_begin); i < update_end; ++ i) {
			Connection &c = connections[i];
			if (c.in_queue())
				queue.update(c.heap_idx);
			else
				queue.push(&c);
		}
	}

	// Flip the segments based on the flip flag.
	for (Polyline &pl : polylines)
		if (connections[&pl - polylines.data()].flipped)
			pl.reverse();

#ifndef NDEBUG
	double cost_final = cost();
#ifdef DEBUG_SVG_OUTPUT
	svg_draw_polyline_chain("improve_ordering_by_segment_flipping-final", iRun, polylines);
#endif /* DEBUG_SVG_OUTPUT */
	assert(cost_final <= cost_prev);
	assert(cost_final <= cost_initial);
#endif /* NDEBUG */
}
#endif

struct FlipEdge {
	FlipEdge(const Vec2d &p1, const Vec2d &p2, size_t source_index) : p1(p1), p2(p2), source_index(source_index) {}
	void	flip() { std::swap(this->p1, this->p2); }
	Vec2d	p1;
	Vec2d	p2;
	size_t	source_index;
};

struct ConnectionCost {
	ConnectionCost(double cost, double cost_flipped) : cost(cost), cost_flipped(cost_flipped) {}
	ConnectionCost() : cost(0.), cost_flipped(0.) {}
	void	flip() { std::swap(this->cost, this->cost_flipped); }
	double	cost = 0;
	double	cost_flipped = 0;
};
static inline ConnectionCost operator-(const ConnectionCost &lhs, const ConnectionCost& rhs) { return ConnectionCost(lhs.cost - rhs.cost, lhs.cost_flipped - rhs.cost_flipped); }

static inline std::pair<double, size_t> minimum_crossover_cost(
	const std::vector<FlipEdge>		  &edges,
	const std::pair<size_t, size_t>   &span1, const ConnectionCost &cost1,
	const std::pair<size_t, size_t>   &span2, const ConnectionCost &cost2,
	const std::pair<size_t, size_t>   &span3, const ConnectionCost &cost3,
	const double					   cost_current)
{
	auto connection_cost = [&edges](
		const std::pair<size_t, size_t> &span1, const ConnectionCost &cost1, bool reversed1, bool flipped1,
		const std::pair<size_t, size_t> &span2, const ConnectionCost &cost2, bool reversed2, bool flipped2,
		const std::pair<size_t, size_t> &span3, const ConnectionCost &cost3, bool reversed3, bool flipped3) {
		auto first_point = [&edges](const std::pair<size_t, size_t> &span, bool flipped) { return flipped ? edges[span.first].p2 : edges[span.first].p1; };
		auto last_point  = [&edges](const std::pair<size_t, size_t> &span, bool flipped) { return flipped ? edges[span.second - 1].p1 : edges[span.second - 1].p2; };
		auto point       = [first_point, last_point](const std::pair<size_t, size_t> &span, bool start, bool flipped) { return start ? first_point(span, flipped) : last_point(span, flipped); };
		auto cost        = [](const ConnectionCost &acost, bool flipped) { 
			assert(acost.cost >= 0. && acost.cost_flipped >= 0.);
			return flipped ? acost.cost_flipped : acost.cost;
		};
		// Ignore reversed single segment spans.
		auto simple_span_ignore = [](const std::pair<size_t, size_t>& span, bool reversed) {
			return span.first + 1 == span.second && reversed;
		};
		assert(span1.first < span1.second);
		assert(span2.first < span2.second);
		assert(span3.first < span3.second);
		return 
			simple_span_ignore(span1, reversed1) || simple_span_ignore(span2, reversed2) || simple_span_ignore(span3, reversed3) ?
				// Don't perform unnecessary calculations simulating reversion of single segment spans.
				std::numeric_limits<double>::max() :
				// Calculate the cost of reverting chains and / or flipping segment orientations.
				cost(cost1, flipped1) + cost(cost2, flipped2) + cost(cost3, flipped3) +
					(point(span2, ! reversed2, flipped2) - point(span1, reversed1, flipped1)).norm() + 
					(point(span3, ! reversed3, flipped3) - point(span2, reversed2, flipped2)).norm();
	};

#ifndef NDEBUG
	{
		double c = connection_cost(span1, cost1, false, false, span2, cost2, false, false, span3, cost3, false, false);
		assert(std::abs(c - cost_current) < SCALED_EPSILON);
	}
#endif /* NDEBUG */

	double cost_min = cost_current;
	size_t flip_min = 0; // no flip, no improvement
	for (size_t i = 0; i < (1 << 6); ++ i) {
		// From the three combinations of 1,2,3 ordering, the other three are reversals of the first three.
		double c1 = (i == 0) ? cost_current : 
			        connection_cost(span1, cost1, (i & 1) != 0, (i & (1 << 1)) != 0, span2, cost2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, cost3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0);
		double c2 = connection_cost(span1, cost1, (i & 1) != 0, (i & (1 << 1)) != 0, span3, cost3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span2, cost2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0);
		double c3 = connection_cost(span2, cost2, (i & 1) != 0, (i & (1 << 1)) != 0, span1, cost1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, cost3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0);
		if (c1 < cost_min) {
			cost_min = c1;
			flip_min = i;
		}
		if (c2 < cost_min) {
			cost_min = c2;
			flip_min = i + (1 << 6);
		}
		if (c3 < cost_min) {
			cost_min = c3;
			flip_min = i + (2 << 6);
		}
	}
	return std::make_pair(cost_min, flip_min);
}

#if 0
static inline std::pair<double, size_t> minimum_crossover_cost(
	const std::vector<FlipEdge>		  &edges,
	const std::pair<size_t, size_t>   &span1, const ConnectionCost &cost1,
	const std::pair<size_t, size_t>   &span2, const ConnectionCost &cost2,
	const std::pair<size_t, size_t>   &span3, const ConnectionCost &cost3,
	const std::pair<size_t, size_t>   &span4, const ConnectionCost &cost4,
	const double					   cost_current)
{
	auto connection_cost = [&edges](
		const std::pair<size_t, size_t> &span1, const ConnectionCost &cost1, bool reversed1, bool flipped1,
		const std::pair<size_t, size_t> &span2, const ConnectionCost &cost2, bool reversed2, bool flipped2,
		const std::pair<size_t, size_t> &span3, const ConnectionCost &cost3, bool reversed3, bool flipped3,
		const std::pair<size_t, size_t> &span4, const ConnectionCost &cost4, bool reversed4, bool flipped4) {
		auto first_point = [&edges](const std::pair<size_t, size_t> &span, bool flipped) { return flipped ? edges[span.first].p2 : edges[span.first].p1; };
		auto last_point  = [&edges](const std::pair<size_t, size_t> &span, bool flipped) { return flipped ? edges[span.second - 1].p1 : edges[span.second - 1].p2; };
		auto point       = [first_point, last_point](const std::pair<size_t, size_t> &span, bool start, bool flipped) { return start ? first_point(span, flipped) : last_point(span, flipped); };
		auto cost        = [](const ConnectionCost &acost, bool flipped) { 
			assert(acost.cost >= 0. && acost.cost_flipped >= 0.);
			return flipped ? acost.cost_flipped : acost.cost;
		};
		// Ignore reversed single segment spans.
		auto simple_span_ignore = [](const std::pair<size_t, size_t>& span, bool reversed) {
			return span.first + 1 == span.second && reversed;
		};
		assert(span1.first < span1.second);
		assert(span2.first < span2.second);
		assert(span3.first < span3.second);
		assert(span4.first < span4.second);
		return 
			simple_span_ignore(span1, reversed1) || simple_span_ignore(span2, reversed2) || simple_span_ignore(span3, reversed3) || simple_span_ignore(span4, reversed4) ?
				// Don't perform unnecessary calculations simulating reversion of single segment spans.
				std::numeric_limits<double>::max() :
				// Calculate the cost of reverting chains and / or flipping segment orientations.
				cost(cost1, flipped1) + cost(cost2, flipped2) + cost(cost3, flipped3) + cost(cost4, flipped4) +
					(point(span2, ! reversed2, flipped2) - point(span1, reversed1, flipped1)).norm() + 
					(point(span3, ! reversed3, flipped3) - point(span2, reversed2, flipped2)).norm() +
					(point(span4, ! reversed4, flipped4) - point(span3, reversed3, flipped3)).norm();
	};

#ifndef NDEBUG
	{
		double c = connection_cost(span1, cost1, false, false, span2, cost2, false, false, span3, cost3, false, false, span4, cost4, false, false);
		assert(std::abs(c - cost_current) < SCALED_EPSILON);
	}
#endif /* NDEBUG */

	double cost_min = cost_current;
	size_t flip_min = 0; // no flip, no improvement
	for (size_t i = 0; i < (1 << 8); ++ i) {
		// From the three combinations of 1,2,3 ordering, the other three are reversals of the first three.
		size_t permutation = 0;
		for (double c : {
				(i == 0) ? cost_current : 
				connection_cost(span1, cost1, (i & 1) != 0, (i & (1 << 1)) != 0, span2, cost2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, cost3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, cost4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span1, cost1, (i & 1) != 0, (i & (1 << 1)) != 0, span2, cost2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span4, cost4, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span3, cost3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span1, cost1, (i & 1) != 0, (i & (1 << 1)) != 0, span3, cost3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span2, cost2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, cost4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span1, cost1, (i & 1) != 0, (i & (1 << 1)) != 0, span3, cost3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span4, cost4, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span2, cost2, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span1, cost1, (i & 1) != 0, (i & (1 << 1)) != 0, span4, cost4, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span2, cost2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span3, cost3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span1, cost1, (i & 1) != 0, (i & (1 << 1)) != 0, span4, cost4, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, cost3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span2, cost2, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span2, cost2, (i & 1) != 0, (i & (1 << 1)) != 0, span1, cost1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, cost3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, cost4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span2, cost2, (i & 1) != 0, (i & (1 << 1)) != 0, span1, cost1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span4, cost4, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span3, cost3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span2, cost2, (i & 1) != 0, (i & (1 << 1)) != 0, span3, cost3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span1, cost1, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, cost4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span2, cost2, (i & 1) != 0, (i & (1 << 1)) != 0, span4, cost4, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span1, cost1, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span3, cost3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span3, cost3, (i & 1) != 0, (i & (1 << 1)) != 0, span1, cost1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span2, cost2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, cost4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(span3, cost3, (i & 1) != 0, (i & (1 << 1)) != 0, span2, cost2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span1, cost1, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, cost4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0)
			}) {
			if (c < cost_min) {
				cost_min = c;
				flip_min = i + (permutation << 8);
			}
			++ permutation;
		}
	}
	return std::make_pair(cost_min, flip_min);
}
#endif

static inline void do_crossover(const std::vector<FlipEdge> &edges_in, std::vector<FlipEdge> &edges_out,
	const std::pair<size_t, size_t> &span1, const std::pair<size_t, size_t> &span2, const std::pair<size_t, size_t> &span3,
	size_t i)
{
	assert(edges_in.size() == edges_out.size());
	auto do_it = [&edges_in, &edges_out](
		const std::pair<size_t, size_t> &span1, bool reversed1, bool flipped1,
		const std::pair<size_t, size_t> &span2, bool reversed2, bool flipped2,
		const std::pair<size_t, size_t> &span3, bool reversed3, bool flipped3) {
		auto it_edges_out = edges_out.begin();
        auto copy_span = [&edges_in, &it_edges_out](std::pair<size_t, size_t> span, bool reversed, bool flipped) {
			assert(span.first < span.second);
			auto it = it_edges_out;
			if (reversed)
				std::reverse_copy(edges_in.begin() + span.first, edges_in.begin() + span.second, it_edges_out);
			else
				std::copy        (edges_in.begin() + span.first, edges_in.begin() + span.second, it_edges_out);
			it_edges_out += span.second - span.first;
			if (reversed != flipped) {
				for (; it != it_edges_out; ++ it)
					it->flip();
			}
		};
		copy_span(span1, reversed1, flipped1);
		copy_span(span2, reversed2, flipped2);
		copy_span(span3, reversed3, flipped3);
	};
	switch (i >> 6) {
	case 0:
		do_it(span1, (i & 1) != 0, (i & (1 << 1)) != 0, span2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0);
		break;
	case 1:
		do_it(span1, (i & 1) != 0, (i & (1 << 1)) != 0, span3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0);
		break;
	default:
		assert((i >> 6) == 2);
		do_it(span2, (i & 1) != 0, (i & (1 << 1)) != 0, span1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0);
	}
	assert(edges_in.size() == edges_out.size());
}

#if 0
static inline void do_crossover(const std::vector<FlipEdge> &edges_in, std::vector<FlipEdge> &edges_out,
	const std::pair<size_t, size_t> &span1, const std::pair<size_t, size_t> &span2, const std::pair<size_t, size_t> &span3, const std::pair<size_t, size_t> &span4,
	size_t i)
{
	assert(edges_in.size() == edges_out.size());
	auto do_it = [&edges_in, &edges_out](
		const std::pair<size_t, size_t> &span1, bool reversed1, bool flipped1,
		const std::pair<size_t, size_t> &span2, bool reversed2, bool flipped2,
		const std::pair<size_t, size_t> &span3, bool reversed3, bool flipped3,
		const std::pair<size_t, size_t> &span4, bool reversed4, bool flipped4) {
		auto it_edges_out = edges_out.begin();
        auto copy_span = [&edges_in, &it_edges_out](std::pair<size_t, size_t> span, bool reversed, bool flipped) {
			assert(span.first < span.second);
			auto it = it_edges_out;
			if (reversed)
				std::reverse_copy(edges_in.begin() + span.first, edges_in.begin() + span.second, it_edges_out);
			else
				std::copy        (edges_in.begin() + span.first, edges_in.begin() + span.second, it_edges_out);
			it_edges_out += span.second - span.first;
			if (reversed != flipped) {
				for (; it != it_edges_out; ++ it)
					it->flip();
			}
		};
		copy_span(span1, reversed1, flipped1);
		copy_span(span2, reversed2, flipped2);
		copy_span(span3, reversed3, flipped3);
		copy_span(span4, reversed4, flipped4);
	};
	switch (i >> 8) {
	case 0:
		assert(i != 0); // otherwise it would be a no-op
		do_it(span1, (i & 1) != 0, (i & (1 << 1)) != 0, span2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 1:
		do_it(span1, (i & 1) != 0, (i & (1 << 1)) != 0, span2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span4, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 2:
		do_it(span1, (i & 1) != 0, (i & (1 << 1)) != 0, span3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 3:
		do_it(span1, (i & 1) != 0, (i & (1 << 1)) != 0, span3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span4, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span2, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 4:
		do_it(span1, (i & 1) != 0, (i & (1 << 1)) != 0, span4, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 5:
		do_it(span1, (i & 1) != 0, (i & (1 << 1)) != 0, span4, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span2, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 6:
		do_it(span2, (i & 1) != 0, (i & (1 << 1)) != 0, span1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 7:
		do_it(span2, (i & 1) != 0, (i & (1 << 1)) != 0, span1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span4, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 8:
		do_it(span2, (i & 1) != 0, (i & (1 << 1)) != 0, span3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span1, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 9:
		do_it(span2, (i & 1) != 0, (i & (1 << 1)) != 0, span4, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span1, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	case 10:
		do_it(span3, (i & 1) != 0, (i & (1 << 1)) != 0, span1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	default:
		assert((i >> 8) == 11);
		do_it(span3, (i & 1) != 0, (i & (1 << 1)) != 0, span2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, span1, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, span4, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0);
		break;
	}
	assert(edges_in.size() == edges_out.size());
}
#endif

// Worst time complexity:    O(min(n, 100) * (n * log n + n^2)
// Expected time complexity: O(min(n, 100) * (n * log n + k * n)
// where n is the number of edges and k is the number of connection_lengths candidates after the first one
// is found that improves the total cost.
//FIXME there are likley better heuristics to lower the time complexity.
static inline void reorder_by_two_exchanges_with_segment_flipping(std::vector<FlipEdge> &edges)
{
	if (edges.size() < 2)
		return;

	std::vector<ConnectionCost> 			connections(edges.size());
	std::vector<FlipEdge> 					edges_tmp(edges);
	std::vector<std::pair<double, size_t>>	connection_lengths(edges.size() - 1, std::pair<double, size_t>(0., 0));
	std::vector<char>						connection_tried(edges.size(), false);
	const size_t 							max_iterations = std::min(edges.size(), size_t(100));
	for (size_t iter = 0; iter < max_iterations; ++ iter) {
		// Initialize connection costs and connection lengths.
		for (size_t i = 1; i < edges.size(); ++ i) {
			const FlipEdge   	 &e1 = edges[i - 1];
			const FlipEdge   	 &e2 = edges[i];
			ConnectionCost	     &c  = connections[i];
			c = connections[i - 1];
			double l = (e2.p1 - e1.p2).norm();
			c.cost += l;
			c.cost_flipped += (e2.p2 - e1.p1).norm();
			connection_lengths[i - 1] = std::make_pair(l, i);
		}
		std::sort(connection_lengths.begin(), connection_lengths.end(), [](const std::pair<double, size_t> &l, const std::pair<double, size_t> &r) { return l.first > r.first; });
		std::fill(connection_tried.begin(), connection_tried.end(), false);
		size_t crossover1_pos_final = std::numeric_limits<size_t>::max();
		size_t crossover2_pos_final = std::numeric_limits<size_t>::max();
		size_t crossover_flip_final = 0;
        for (const std::pair<double, size_t>& first_crossover_candidate : connection_lengths) {
            size_t longest_connection_idx = first_crossover_candidate.second;
			connection_tried[longest_connection_idx] = true;
			// Find the second crossover connection with the lowest total chain cost.
			size_t crossover_pos_min  = std::numeric_limits<size_t>::max();
			double crossover_cost_min = connections.back().cost;
			size_t crossover_flip_min = 0;
			for (size_t j = 1; j < connections.size(); ++ j)
				if (! connection_tried[j]) {
					size_t a = j;
					size_t b = longest_connection_idx;
					if (a > b)
						std::swap(a, b);
					std::pair<double, size_t> cost_and_flip = minimum_crossover_cost(edges, 
						std::make_pair(size_t(0), a), connections[a - 1], std::make_pair(a, b), connections[b - 1] - connections[a], std::make_pair(b, edges.size()), connections.back() - connections[b],
						connections.back().cost);
					if (cost_and_flip.second > 0 && cost_and_flip.first < crossover_cost_min) {
						crossover_pos_min  = j;
						crossover_cost_min = cost_and_flip.first;
						crossover_flip_min = cost_and_flip.second;
						assert(crossover_cost_min < connections.back().cost + EPSILON);
					}
				}
			if (crossover_cost_min < connections.back().cost) {
				// The cost of the chain with the proposed two crossovers has a lower total cost than the current chain. Apply the crossover.
				crossover1_pos_final = longest_connection_idx;
				crossover2_pos_final = crossover_pos_min;
				crossover_flip_final = crossover_flip_min;
				break;
			} else {
				// Continue with another long candidate edge.
			}
		}
		if (crossover_flip_final > 0) {
			// Pair of cross over positions and flip / reverse constellation has been found, which improves the total cost of the connection.
			// Perform a crossover.
			if (crossover1_pos_final > crossover2_pos_final)
				std::swap(crossover1_pos_final, crossover2_pos_final);
			do_crossover(edges, edges_tmp, std::make_pair(size_t(0), crossover1_pos_final), std::make_pair(crossover1_pos_final, crossover2_pos_final), std::make_pair(crossover2_pos_final, edges.size()), crossover_flip_final);
			edges.swap(edges_tmp);
		} else {
			// No valid pair of cross over positions was found improving the total cost. Giving up.
			break;
		}
	}
}

#if 0
// Currently not used, too slow.
static inline void reorder_by_three_exchanges_with_segment_flipping(std::vector<FlipEdge> &edges)
{
	if (edges.size() < 3) {
		reorder_by_two_exchanges_with_segment_flipping(edges);
		return;
	}

	std::vector<ConnectionCost> 			connections(edges.size());
	std::vector<FlipEdge> 					edges_tmp(edges);
	std::vector<std::pair<double, size_t>>	connection_lengths(edges.size() - 1, std::pair<double, size_t>(0., 0));
	std::vector<char>						connection_tried(edges.size(), false);
	for (size_t iter = 0; iter < edges.size(); ++ iter) {
		// Initialize connection costs and connection lengths.
		for (size_t i = 1; i < edges.size(); ++ i) {
			const FlipEdge   	 &e1 = edges[i - 1];
			const FlipEdge   	 &e2 = edges[i];
			ConnectionCost	     &c  = connections[i];
			c = connections[i - 1];
			double l = (e2.p1 - e1.p2).norm();
			c.cost += l;
			c.cost_flipped += (e2.p2 - e1.p1).norm();
			connection_lengths[i - 1] = std::make_pair(l, i);
		}
		std::sort(connection_lengths.begin(), connection_lengths.end(), [](const std::pair<double, size_t> &l, const std::pair<double, size_t> &r) { return l.first > r.first; });
		std::fill(connection_tried.begin(), connection_tried.end(), false);
		size_t crossover1_pos_final = std::numeric_limits<size_t>::max();
		size_t crossover2_pos_final = std::numeric_limits<size_t>::max();
		size_t crossover3_pos_final = std::numeric_limits<size_t>::max();
		size_t crossover_flip_final = 0;
        for (const std::pair<double, size_t> &first_crossover_candidate : connection_lengths) {
            size_t longest_connection_idx = first_crossover_candidate.second;
            connection_tried[longest_connection_idx] = true;
			// Find the second crossover connection with the lowest total chain cost.
			double crossover_cost_min = connections.back().cost;
			for (size_t j = 1; j < connections.size(); ++ j)
				if (! connection_tried[j]) {
					for (size_t k = j + 1; k < connections.size(); ++ k)
						if (! connection_tried[k]) {
							size_t a = longest_connection_idx;
							size_t b = j;
							size_t c = k;
							if (a > c)
								std::swap(a, c);
							if (a > b)
								std::swap(a, b);
							if (b > c)
								std::swap(b, c);
							std::pair<double, size_t> cost_and_flip = minimum_crossover_cost(edges, 
								std::make_pair(size_t(0), a), connections[a - 1], std::make_pair(a, b), connections[b - 1] - connections[a], 
								std::make_pair(b, c), connections[c - 1] - connections[b], std::make_pair(c, edges.size()), connections.back() - connections[c],
								connections.back().cost);
							if (cost_and_flip.second > 0 && cost_and_flip.first < crossover_cost_min) {
								crossover_cost_min   = cost_and_flip.first;
								crossover1_pos_final = a;
								crossover2_pos_final = b;
								crossover3_pos_final = c;
								crossover_flip_final = cost_and_flip.second;
								assert(crossover_cost_min < connections.back().cost + EPSILON);
							}
						}
				}
			if (crossover_flip_final > 0) {
				// The cost of the chain with the proposed two crossovers has a lower total cost than the current chain. Apply the crossover.
				break;
			} else {
				// Continue with another long candidate edge.
			}
		}
		if (crossover_flip_final > 0) {
			// Pair of cross over positions and flip / reverse constellation has been found, which improves the total cost of the connection.
			// Perform a crossover.
			do_crossover(edges, edges_tmp, std::make_pair(size_t(0), crossover1_pos_final), std::make_pair(crossover1_pos_final, crossover2_pos_final), 
				std::make_pair(crossover2_pos_final, crossover3_pos_final), std::make_pair(crossover3_pos_final, edges.size()), crossover_flip_final);
			edges.swap(edges_tmp);
		} else {
			// No valid pair of cross over positions was found improving the total cost. Giving up.
			break;
		}
	}
}
#endif

typedef Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::DontAlign> Matrixd;

class FourOptCosts {
public:
	FourOptCosts(const ConnectionCost &c1, const ConnectionCost &c2, const ConnectionCost &c3, const ConnectionCost &c4) : costs { &c1, &c2, &c3, &c4 } {}

	double operator()(size_t piece_idx, bool flipped) const { return flipped ? costs[piece_idx]->cost_flipped : costs[piece_idx]->cost; }

private:
	const ConnectionCost* costs[4];
};

#if 0
static inline std::pair<double, size_t> minimum_crossover_cost(
	const FourOptCosts				  &segment_costs,
	const Matrixd 					  &segment_end_point_distance_matrix,
	const double					   cost_current)
{
	// Distance from the end of span1 to the start of span2.
	auto end_point_distance = [&segment_end_point_distance_matrix](size_t span1, bool reversed1, bool flipped1, size_t span2, bool reversed2, bool flipped2) {
		return segment_end_point_distance_matrix(span1 * 4 + (! reversed1) * 2 + flipped1, span2 * 4 + reversed2 * 2 + flipped2);
	};
	auto connection_cost = [&segment_costs, end_point_distance](
		const size_t span1, bool reversed1, bool flipped1,
		const size_t span2, bool reversed2, bool flipped2,
		const size_t span3, bool reversed3, bool flipped3,
		const size_t span4, bool reversed4, bool flipped4) {
		// Calculate the cost of reverting chains and / or flipping segment orientations.
		return segment_costs(span1, flipped1) + segment_costs(span2, flipped2) + segment_costs(span3, flipped3) + segment_costs(span4, flipped4) +
			   end_point_distance(span1, reversed1, flipped1, span2, reversed2, flipped2) +
			   end_point_distance(span2, reversed2, flipped2, span3, reversed3, flipped3) +
			   end_point_distance(span3, reversed3, flipped3, span4, reversed4, flipped4);
	};

#ifndef NDEBUG
	{
		double c = connection_cost(0, false, false, 1, false, false, 2, false, false, 3, false, false);
		assert(std::abs(c - cost_current) < SCALED_EPSILON);
	}
#endif /* NDEBUG */

	double cost_min = cost_current;
	size_t flip_min = 0; // no flip, no improvement
	for (size_t i = 0; i < (1 << 8); ++ i) {
		// From the three combinations of 1,2,3 ordering, the other three are reversals of the first three.
		size_t permutation = 0;
		for (double c : {
				(i == 0) ? cost_current : 
				connection_cost(0, (i & 1) != 0, (i & (1 << 1)) != 0, 1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(0, (i & 1) != 0, (i & (1 << 1)) != 0, 1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 2, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(0, (i & 1) != 0, (i & (1 << 1)) != 0, 2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 1, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(0, (i & 1) != 0, (i & (1 << 1)) != 0, 2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 1, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(0, (i & 1) != 0, (i & (1 << 1)) != 0, 3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 1, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 2, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(0, (i & 1) != 0, (i & (1 << 1)) != 0, 3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 1, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(1, (i & 1) != 0, (i & (1 << 1)) != 0, 0, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 2, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(1, (i & 1) != 0, (i & (1 << 1)) != 0, 0, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 3, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 2, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(1, (i & 1) != 0, (i & (1 << 1)) != 0, 2, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 0, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(1, (i & 1) != 0, (i & (1 << 1)) != 0, 3, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 0, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 2, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(2, (i & 1) != 0, (i & (1 << 1)) != 0, 0, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 1, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0),
				connection_cost(2, (i & 1) != 0, (i & (1 << 1)) != 0, 1, (i & (1 << 2)) != 0, (i & (1 << 3)) != 0, 0, (i & (1 << 4)) != 0, (i & (1 << 5)) != 0, 3, (i & (1 << 6)) != 0, (i & (1 << 7)) != 0)
			}) {
			if (c < cost_min) {
				cost_min = c;
				flip_min = i + (permutation << 8);
			}
			++ permutation;
		}
	}
	return std::make_pair(cost_min, flip_min);
}

// Currently not used, too slow.
static inline void reorder_by_three_exchanges_with_segment_flipping2(std::vector<FlipEdge> &edges)
{
	if (edges.size() < 3) {
		reorder_by_two_exchanges_with_segment_flipping(edges);
		return;
	}

	std::vector<ConnectionCost> 			connections(edges.size());
	std::vector<FlipEdge> 					edges_tmp(edges);
	std::vector<std::pair<double, size_t>>	connection_lengths(edges.size() - 1, std::pair<double, size_t>(0., 0));
	std::vector<char>						connection_tried(edges.size(), false);
	for (size_t iter = 0; iter < edges.size(); ++ iter) {
		// Initialize connection costs and connection lengths.
		for (size_t i = 1; i < edges.size(); ++ i) {
			const FlipEdge   	 &e1 = edges[i - 1];
			const FlipEdge   	 &e2 = edges[i];
			ConnectionCost	     &c  = connections[i];
			c = connections[i - 1];
			double l = (e2.p1 - e1.p2).norm();
			c.cost += l;
			c.cost_flipped += (e2.p2 - e1.p1).norm();
			connection_lengths[i - 1] = std::make_pair(l, i);
		}
		std::sort(connection_lengths.begin(), connection_lengths.end(), [](const std::pair<double, size_t> &l, const std::pair<double, size_t> &r) { return l.first > r.first; });
		std::fill(connection_tried.begin(), connection_tried.end(), false);
		size_t crossover1_pos_final = std::numeric_limits<size_t>::max();
		size_t crossover2_pos_final = std::numeric_limits<size_t>::max();
		size_t crossover3_pos_final = std::numeric_limits<size_t>::max();
		size_t crossover_flip_final = 0;
		// Distances between the end points of the four pieces of the current segment sequence.
#ifdef NDEBUG
		Matrixd segment_end_point_distance_matrix(4 * 4, 4 * 4);
#else /* NDEBUG */
		Matrixd segment_end_point_distance_matrix = Matrixd::Constant(4 * 4, 4 * 4, std::numeric_limits<double>::max());
#endif /* NDEBUG */
        for (const std::pair<double, size_t> &first_crossover_candidate : connection_lengths) {
            size_t longest_connection_idx = first_crossover_candidate.second;
            connection_tried[longest_connection_idx] = true;
            // Find the second crossover connection with the lowest total chain cost.
			double crossover_cost_min = connections.back().cost;
			for (size_t j = 1; j < connections.size(); ++ j)
				if (! connection_tried[j]) {
					for (size_t k = j + 1; k < connections.size(); ++ k)
						if (! connection_tried[k]) {
							size_t a = longest_connection_idx;
							size_t b = j;
							size_t c = k;
							if (a > c)
								std::swap(a, c);
							if (a > b)
								std::swap(a, b);
							if (b > c)
								std::swap(b, c);
							const Vec2d* endpts[16] = {
								&edges[0].p1, &edges[0].p2, &edges[a - 1].p2, &edges[a - 1].p1,
								&edges[a].p1, &edges[a].p2, &edges[b - 1].p2, &edges[b - 1].p1,
								&edges[b].p1, &edges[b].p2, &edges[c - 1].p2, &edges[c - 1].p1,
								&edges[c].p1, &edges[c].p2, &edges.back().p2, &edges.back().p1 };
							for (size_t v = 0; v < 16; ++ v) {
								const Vec2d &p1 = *endpts[v];
								for (size_t u = (v & (~3)) + 4; u < 16; ++ u)
									segment_end_point_distance_matrix(u, v) = segment_end_point_distance_matrix(v, u) = (*endpts[u] - p1).norm();
							}
							FourOptCosts segment_costs(connections[a - 1], connections[b - 1] - connections[a], connections[c - 1] - connections[b],  connections.back() - connections[c]);
							std::pair<double, size_t> cost_and_flip = minimum_crossover_cost(segment_costs, segment_end_point_distance_matrix, connections.back().cost);
							if (cost_and_flip.second > 0 && cost_and_flip.first < crossover_cost_min) {
								crossover_cost_min   = cost_and_flip.first;
								crossover1_pos_final = a;
								crossover2_pos_final = b;
								crossover3_pos_final = c;
								crossover_flip_final = cost_and_flip.second;
								assert(crossover_cost_min < connections.back().cost + EPSILON);
							}
						}
				}
			if (crossover_flip_final > 0) {
				// The cost of the chain with the proposed two crossovers has a lower total cost than the current chain. Apply the crossover.
				break;
			} else {
				// Continue with another long candidate edge.
			}
		}
		if (crossover_flip_final > 0) {
			// Pair of cross over positions and flip / reverse constellation has been found, which improves the total cost of the connection.
			// Perform a crossover.
			do_crossover(edges, edges_tmp, std::make_pair(size_t(0), crossover1_pos_final), std::make_pair(crossover1_pos_final, crossover2_pos_final), 
				std::make_pair(crossover2_pos_final, crossover3_pos_final), std::make_pair(crossover3_pos_final, edges.size()), crossover_flip_final);
			edges.swap(edges_tmp);
		} else {
			// No valid pair of cross over positions was found improving the total cost. Giving up.
			break;
		}
	}
}
#endif

// Flip the sequences of polylines to lower the total length of connecting lines.
// Used by the infill generator if the infill is not connected with perimeter lines
// and to order the brim lines.
static inline void improve_ordering_by_two_exchanges_with_segment_flipping(Polylines &polylines, bool fixed_start)
{
#ifndef NDEBUG
	auto cost = [&polylines]() {
		double sum = 0.;
		for (size_t i = 1; i < polylines.size(); ++i)
			sum += (polylines[i].first_point() - polylines[i - 1].last_point()).cast<double>().norm();
		return sum;
	};
	double cost_initial = cost();

	static int iRun = 0;
	++ iRun;
#ifdef DEBUG_SVG_OUTPUT
	svg_draw_polyline_chain("improve_ordering_by_two_exchanges_with_segment_flipping-initial", iRun, polylines);
#endif /* DEBUG_SVG_OUTPUT */
#endif /* NDEBUG */

	std::vector<FlipEdge> edges;
	edges.reserve(polylines.size());
    std::transform(polylines.begin(), polylines.end(), std::back_inserter(edges), 
    	[&polylines](const Polyline &pl){ return FlipEdge(pl.first_point().cast<double>(), pl.last_point().cast<double>(), &pl - polylines.data()); });
#if 1
	reorder_by_two_exchanges_with_segment_flipping(edges);
#else
	// reorder_by_three_exchanges_with_segment_flipping(edges);
	reorder_by_three_exchanges_with_segment_flipping2(edges);
#endif
	Polylines out;
	out.reserve(polylines.size());
	for (const FlipEdge &edge : edges) {
		Polyline &pl = polylines[edge.source_index];
		out.emplace_back(std::move(pl));
		if (edge.p2 == out.back().first_point().cast<double>()) {
			// Polyline is flipped.
			out.back().reverse();
		} else {
			// Polyline is not flipped.
			assert(edge.p1 == out.back().first_point().cast<double>());
		}
	}
	polylines = out;

#ifndef NDEBUG
	double cost_final = cost();
#ifdef DEBUG_SVG_OUTPUT
	svg_draw_polyline_chain("improve_ordering_by_two_exchanges_with_segment_flipping-final", iRun, out);
#endif /* DEBUG_SVG_OUTPUT */
	assert(cost_final <= cost_initial);
#endif /* NDEBUG */
}

// Used to optimize order of infill lines and brim lines.
Polylines chain_polylines(Polylines &&polylines, const Point *start_near)
{
#ifdef DEBUG_SVG_OUTPUT
	static int iRun = 0;
	++ iRun;
	svg_draw_polyline_chain("chain_polylines-initial", iRun, polylines);
#endif /* DEBUG_SVG_OUTPUT */

	Polylines out;
	if (! polylines.empty()) {
		auto segment_end_point = [&polylines](size_t idx, bool first_point) -> const Point& { return first_point ? polylines[idx].first_point() : polylines[idx].last_point(); };
		std::vector<std::pair<size_t, bool>> ordered = chain_segments_greedy2<Point, decltype(segment_end_point)>(segment_end_point, polylines.size(), start_near);
		out.reserve(polylines.size()); 
		for (auto &segment_and_reversal : ordered) {
			out.emplace_back(std::move(polylines[segment_and_reversal.first]));
			if (segment_and_reversal.second)
				out.back().reverse();
		}
		if (out.size() > 1 && start_near == nullptr) {
			improve_ordering_by_two_exchanges_with_segment_flipping(out, start_near != nullptr);
			//improve_ordering_by_segment_flipping(out, start_near != nullptr);
		}
	}

#ifdef DEBUG_SVG_OUTPUT
	svg_draw_polyline_chain("chain_polylines-final", iRun, out);
#endif /* DEBUG_SVG_OUTPUT */
	return out;
}

template<class T> static inline T chain_path_items(const Points &points, const T &items)
{
	auto segment_end_point = [&points](size_t idx, bool /* first_point */) -> const Point& { return points[idx]; };
	std::vector<std::pair<size_t, bool>> ordered = chain_segments_greedy<Point, decltype(segment_end_point)>(segment_end_point, points.size(), nullptr);
	T out;
	out.reserve(items.size());
	for (auto &segment_and_reversal : ordered)
		out.emplace_back(items[segment_and_reversal.first]);
	return out;
}

ClipperLib::PolyNodes chain_clipper_polynodes(const Points &points, const ClipperLib::PolyNodes &items)
{
	return chain_path_items(points, items);
}

// BBS
std::vector<const PrintInstance*> chain_print_object_instances(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
	// Order objects using a nearest neighbor search.
	Points object_reference_points;
	std::vector<std::pair<size_t, size_t>> instances;
	for (size_t i = 0; i < print_objects.size(); ++i) {
		const PrintObject& object = *print_objects[i];
		for (size_t j = 0; j < object.instances().size(); ++j) {
			// Sliced PrintObjects are centered, object.instances()[j].shift is the center of the PrintObject in G-code coordinates.
			object_reference_points.emplace_back(object.instances()[j].shift);
			instances.emplace_back(i, j);
		}
	}
	auto segment_end_point = [&object_reference_points](size_t idx, bool /* first_point */) -> const Point& { return object_reference_points[idx]; };
	std::vector<std::pair<size_t, bool>> ordered = chain_segments_greedy<Point, decltype(segment_end_point)>(segment_end_point, instances.size(), start_near);
	std::vector<const PrintInstance*> out;
	out.reserve(instances.size());
	for (auto& segment_and_reversal : ordered) {
		const std::pair<size_t, size_t>& inst = instances[segment_and_reversal.first];
		out.emplace_back(&print_objects[inst.first]->instances()[inst.second]);
	}
	return out;
}

std::vector<const PrintInstance*> chain_print_object_instances(const Print &print)
{
	return chain_print_object_instances(print.objects().vector(), nullptr);
}

Polylines chain_lines(const std::vector<Line> &lines, const double point_distance_epsilon)
{
    // Create line end point lookup.
    struct LineEnd {
        LineEnd(const Line *line, bool start) : line(line), start(start) {}
        const Line      *line;
        // Is it the start or end point?
        bool             start;
        const Point&     point() const { return start ? line->a : line->b; }
        const Point&     other_point() const { return start ? line->b : line->a; }
        LineEnd          other_end() const { return LineEnd(line, ! start); }
        bool operator==(const LineEnd &rhs) const { return this->line == rhs.line && this->start == rhs.start; }
    };
    struct LineEndAccessor {
        const Point* operator()(const LineEnd &pt) const { return &pt.point(); }
    };
    typedef ClosestPointInRadiusLookup<LineEnd, LineEndAccessor> ClosestPointLookupType;
    ClosestPointLookupType closest_end_point_lookup(point_distance_epsilon);
    for (const Line &line : lines) {
        closest_end_point_lookup.insert(LineEnd(&line, true));
        closest_end_point_lookup.insert(LineEnd(&line, false));
    }

    // Chain the lines.
    std::vector<char> line_consumed(lines.size(), false);
    static const double point_distance_epsilon2 = point_distance_epsilon * point_distance_epsilon;
    Polylines out;
    for (const Line &seed : lines)
        if (! line_consumed[&seed - lines.data()]) {
            line_consumed[&seed - lines.data()] = true;
            closest_end_point_lookup.erase(LineEnd(&seed, false));
            closest_end_point_lookup.erase(LineEnd(&seed, true));
            Polyline pl { seed.a, seed.b };
            for (size_t round = 0; round < 2; ++ round) {
                for (;;) {
                    auto [line_end, dist2] = closest_end_point_lookup.find(pl.last_point());
                    if (line_end == nullptr || dist2 >= point_distance_epsilon2)
                        // Cannot extent in this direction.
                        break;
                    // Average the last point.
                    pl.points.back() = (0.5 * (pl.points.back().cast<double>() + line_end->point().cast<double>())).cast<coord_t>();
                    // and extend with the new line segment.
                    pl.points.emplace_back(line_end->other_point());
                    closest_end_point_lookup.erase(*line_end);
                    closest_end_point_lookup.erase(line_end->other_end());
                    line_consumed[line_end->line - lines.data()] = true;
                }
                // reverse and try the oter direction.
                pl.reverse();
            }
            out.emplace_back(std::move(pl));
        }
    return out;
}

} // namespace Slic3r
