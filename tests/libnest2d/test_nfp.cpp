#include <catch2/catch_all.hpp>

#include "libnest2d_test_utils.hpp"

using namespace libnest2d;

namespace {

struct ItemPair {
    Item orbiter;
    Item stationary;
};

std::vector<ItemPair> nfp_testdata = {
    {
        {
            {80, 50},
            {100, 70},
            {120, 50}
        },
        {
            {10, 10},
            {10, 40},
            {40, 40},
            {40, 10}
        }
    },
    {
        {
            {80, 50},
            {60, 70},
            {80, 90},
            {120, 90},
            {140, 70},
            {120, 50}
        },
        {
            {10, 10},
            {10, 40},
            {40, 40},
            {40, 10}
        }
    },
    {
        {
            {40, 10},
            {30, 10},
            {20, 20},
            {20, 30},
            {30, 40},
            {40, 40},
            {50, 30},
            {50, 20}
        },
        {
            {80, 0},
            {80, 30},
            {110, 30},
            {110, 0}
        }
    },
    {
        {
            {117, 107},
            {118, 109},
            {120, 112},
            {122, 113},
            {128, 113},
            {130, 112},
            {132, 109},
            {133, 107},
            {133, 103},
            {132, 101},
            {130, 98},
            {128, 97},
            {122, 97},
            {120, 98},
            {118, 101},
            {117, 103}
        },
        {
            {102, 116},
            {111, 126},
            {114, 126},
            {144, 106},
            {148, 100},
            {148, 85},
            {147, 84},
            {102, 84}
        }
    },
    {
        {
            {99, 122},
            {108, 140},
            {110, 142},
            {139, 142},
            {151, 122},
            {151, 102},
            {142, 70},
            {139, 68},
            {111, 68},
            {108, 70},
            {99, 102}
        },
        {
            {107, 124},
            {128, 125},
            {133, 125},
            {136, 124},
            {140, 121},
            {142, 119},
            {143, 116},
            {143, 109},
            {141, 93},
            {139, 89},
            {136, 86},
            {134, 85},
            {108, 85},
            {107, 86}
        }
    },
    {
        {
            {91, 100},
            {94, 144},
            {117, 153},
            {118, 153},
            {159, 112},
            {159, 110},
            {156, 66},
            {133, 57},
            {132, 57},
            {91, 98}
        },
        {
            {101, 90},
            {103, 98},
            {107, 113},
            {114, 125},
            {115, 126},
            {135, 126},
            {136, 125},
            {144, 114},
            {149, 90},
            {149, 89},
            {148, 87},
            {145, 84},
            {105, 84},
            {102, 87},
            {101, 89}
        }
    }
};

// libnest2d's vertex order depends on the backend; normalise to clockwise.
Item reversed_if_ccw(Item it) {
    if (!is_clockwise<PolygonImpl>()) {
        auto raw = it.rawShape();
        std::reverse(sl::begin(raw), sl::end(raw));
        it = Item{raw};
    }
    return it;
}

// Sliding `orbiter` around `stationary` along their no-fit polygon must keep the
// two shapes touching at every NFP vertex, and `stationary` must lie inside the
// resulting inner-fit polygon.
template<nfp::NfpLevel lvl, Coord SCALE>
void check_nfp(const std::vector<ItemPair> &testdata) {
    auto check_pair = [](Item orbiter, Item stationary) {
        orbiter.translate({210 * SCALE, 0});

        auto &&nfp = nfp::noFitPolygon<lvl>(stationary.rawShape(), orbiter.transformedShape());
        placers::correctNfpPosition(nfp, stationary, orbiter);
        REQUIRE(shapelike::isValid(nfp.first).first);

        Item infp(nfp.first);
        REQUIRE(stationary.isInside(infp));

        auto vo = nfp::referenceVertex(orbiter.transformedShape());
        for (auto v : infp) {
            Item moved = orbiter;
            moved.translate({getX(v) - getX(vo), getY(v) - getY(vo)});
            REQUIRE(Item::touches(moved, stationary));
        }
    };

    for (const ItemPair &td : testdata) {
        check_pair(reversed_if_ccw(td.orbiter), reversed_if_ccw(td.stationary));
        check_pair(reversed_if_ccw(td.stationary), reversed_if_ccw(td.orbiter));
    }
}

} // namespace

TEST_CASE("No-fit polygon of convex shapes keeps the items touching", "[Geometry][NFP]") {
    check_nfp<nfp::NfpLevel::CONVEX_ONLY, 1>(nfp_testdata);
}

TEST_CASE("BottomLeftPlacer left and down polygons", "[Geometry][NFP]") {
    Box              bin(100, 100);
    BottomLeftPlacer placer(bin);

    PathImpl pitem        = {{70, 75}, {88, 60}, {65, 50}, {60, 30}, {80, 20},
                             {42, 20}, {35, 35}, {35, 55}, {40, 75}};
    PathImpl left_control  = {{40, 75}, {35, 55}, {35, 35}, {42, 20}, {0, 20}, {0, 75}};
    PathImpl down_control  = {{88, 60}, {88, 0}, {35, 0}, {35, 35},
                              {42, 20}, {80, 20}, {60, 30}, {65, 50}};

    if constexpr (!is_clockwise<PathImpl>()) {
        std::reverse(sl::begin(pitem), sl::end(pitem));
        std::reverse(sl::begin(left_control), sl::end(left_control));
        std::reverse(sl::begin(down_control), sl::end(down_control));
    }
    if constexpr (ClosureTypeV<PathImpl> == Closure::CLOSED) {
        sl::addVertex(pitem, sl::front(pitem));
        sl::addVertex(left_control, sl::front(left_control));
        sl::addVertex(down_control, sl::front(down_control));
    }

    auto require_same_vertices = [](const Item &got, const Item &expected) {
        REQUIRE(shapelike::isValid(got.rawShape()).first);
        REQUIRE(got.vertexCount() == expected.vertexCount());
        for (unsigned long i = 0; i < expected.vertexCount(); ++i) {
            REQUIRE(getX(got.vertex(i)) == getX(expected.vertex(i)));
            REQUIRE(getY(got.vertex(i)) == getY(expected.vertex(i)));
        }
    };

    Item item{pitem};
    require_same_vertices(Item(placer.leftPoly(item)), Item{left_control});
    require_same_vertices(Item(placer.downPoly(item)), Item{down_control});
}

TEST_CASE("EdgeCache maps a parameter to a contour point", "[Geometry][NFP]") {
    RectangleItem                   input(10, 10);
    placers::EdgeCache<PolygonImpl> ecache(input);

    auto first = *input.begin();
    REQUIRE(getX(first) == getX(ecache.coords(0)));
    REQUIRE(getY(first) == getY(ecache.coords(0)));

    auto last = *std::prev(input.end());
    REQUIRE(getX(last) == getX(ecache.coords(1.0)));
    REQUIRE(getY(last) == getY(ecache.coords(1.0)));

    for (int i = 0; i <= 100; ++i)
        REQUIRE(shapelike::touches(ecache.coords(i * 0.01), input.transformedShape()));
}

TEST_CASE("Merging a pile with a polygon", "[Geometry][NFP]") {
    RectangleItem rect1(10, 15), rect2(15, 15), rect3(20, 15);
    rect2.translate({10, 0});
    rect3.translate({25, 0});

    TMultiShape<PolygonImpl> pile;
    pile.push_back(rect1.transformedShape());
    pile.push_back(rect2.transformedShape());

    auto result = nfp::merge(pile, rect3.transformedShape());
    REQUIRE(result.size() == 1);   // the three abutting rectangles merge into one

    RectangleItem ref(45, 15);
    REQUIRE_THAT(shapelike::area(result.front()),
                 Catch::Matchers::WithinRel(ref.area(), 1e-9));
}
