#include "SpatIndex.hpp"

// for concave hull merging decisions
#include <libslic3r/SLA/BoostAdapter.hpp>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4267)
#endif

#include "boost/geometry/index/rtree.hpp"

#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace Slic3r { namespace sla {

/* **************************************************************************
 * PointIndex implementation
 * ************************************************************************** */

class PointIndex::Impl {
public:
    using BoostIndex = boost::geometry::index::rtree< PointIndexEl,
                                                     boost::geometry::index::rstar<16, 4> /* ? */ >;

    BoostIndex m_store;
};

PointIndex::PointIndex(): m_impl(new Impl()) {}
PointIndex::~PointIndex() {}

PointIndex::PointIndex(const PointIndex &cpy): m_impl(new Impl(*cpy.m_impl)) {}
PointIndex::PointIndex(PointIndex&& cpy): m_impl(std::move(cpy.m_impl)) {}

PointIndex& PointIndex::operator=(const PointIndex &cpy)
{
    m_impl.reset(new Impl(*cpy.m_impl));
    return *this;
}

PointIndex& PointIndex::operator=(PointIndex &&cpy)
{
    m_impl.swap(cpy.m_impl);
    return *this;
}

void PointIndex::insert(const PointIndexEl &el)
{
    m_impl->m_store.insert(el);
}

bool PointIndex::remove(const PointIndexEl& el)
{
    return m_impl->m_store.remove(el) == 1;
}

std::vector<PointIndexEl>
PointIndex::query(std::function<bool(const PointIndexEl &)> fn) const
{
    namespace bgi = boost::geometry::index;

    std::vector<PointIndexEl> ret;
    m_impl->m_store.query(bgi::satisfies(fn), std::back_inserter(ret));
    return ret;
}

std::vector<PointIndexEl> PointIndex::nearest(const Vec3d &el, unsigned k = 1) const
{
    namespace bgi = boost::geometry::index;
    std::vector<PointIndexEl> ret; ret.reserve(k);
    m_impl->m_store.query(bgi::nearest(el, k), std::back_inserter(ret));
    return ret;
}

size_t PointIndex::size() const
{
    return m_impl->m_store.size();
}

void PointIndex::foreach(std::function<void (const PointIndexEl &)> fn)
{
    for(auto& el : m_impl->m_store) fn(el);
}

void PointIndex::foreach(std::function<void (const PointIndexEl &)> fn) const
{
    for(const auto &el : m_impl->m_store) fn(el);
}

/* **************************************************************************
 * BoxIndex implementation
 * ************************************************************************** */

class BoxIndex::Impl {
public:
    using BoostIndex = boost::geometry::index::
        rtree<BoxIndexEl, boost::geometry::index::rstar<16, 4> /* ? */>;

    BoostIndex m_store;
};

BoxIndex::BoxIndex(): m_impl(new Impl()) {}
BoxIndex::~BoxIndex() {}

BoxIndex::BoxIndex(const BoxIndex &cpy): m_impl(new Impl(*cpy.m_impl)) {}
BoxIndex::BoxIndex(BoxIndex&& cpy): m_impl(std::move(cpy.m_impl)) {}

BoxIndex& BoxIndex::operator=(const BoxIndex &cpy)
{
    m_impl.reset(new Impl(*cpy.m_impl));
    return *this;
}

BoxIndex& BoxIndex::operator=(BoxIndex &&cpy)
{
    m_impl.swap(cpy.m_impl);
    return *this;
}

void BoxIndex::insert(const BoxIndexEl &el)
{
    m_impl->m_store.insert(el);
}

bool BoxIndex::remove(const BoxIndexEl& el)
{
    return m_impl->m_store.remove(el) == 1;
}

std::vector<BoxIndexEl> BoxIndex::query(const BoundingBox &qrbb,
                                        BoxIndex::QueryType qt)
{
    namespace bgi = boost::geometry::index;

    std::vector<BoxIndexEl> ret; ret.reserve(m_impl->m_store.size());

    switch (qt) {
    case qtIntersects:
        m_impl->m_store.query(bgi::intersects(qrbb), std::back_inserter(ret));
        break;
    case qtWithin:
        m_impl->m_store.query(bgi::within(qrbb), std::back_inserter(ret));
    }

    return ret;
}

size_t BoxIndex::size() const
{
    return m_impl->m_store.size();
}

void BoxIndex::foreach(std::function<void (const BoxIndexEl &)> fn)
{
    for(auto& el : m_impl->m_store) fn(el);
}

}} // namespace Slic3r::sla
