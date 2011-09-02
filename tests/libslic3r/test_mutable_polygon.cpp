#include <catch2/catch_all.hpp>

#include "libslic3r/Point.hpp"
#include "libslic3r/MutablePolygon.hpp"

using namespace Slic3r;

SCENARIO("Iterators", "[MutablePolygon]") {
    GIVEN("Polygon with three points") {
        Slic3r::MutablePolygon p({ { 0, 0 }, { 0, 1 }, { 1, 0 } });
        WHEN("Iterating upwards") {
            auto begin = p.begin();
            auto end   = p.end();
            auto it    = begin;
            THEN("++ it is not equal to begin") { 
                REQUIRE(++ it != begin);
            } THEN("++ it is not equal to end")   { 
                REQUIRE(++ it != end);
            } THEN("++ (++ it) is not equal to begin") { 
                REQUIRE(++ (++ it) != begin);
            } THEN("++ (++ it) is equal to end") { 
                REQUIRE(++ (++ it) == end);
            } THEN("++ (++ (++ it)) is equal to begin") { 
                REQUIRE(++ (++ (++ it)) == begin);
            } THEN("++ (++ (++ it)) is not equal to end") { 
                REQUIRE(++ (++ (++ it)) != end);
            } 
        }
        WHEN("Iterating downwards") {
            auto begin = p.begin();
            auto end = p.end();
            auto it = begin;
            THEN("-- it is not equal to begin") {
                REQUIRE(-- it != begin);
            } THEN("-- it is equal to end") {
                REQUIRE(-- it == end);
            } THEN("-- (-- it) is not equal to begin") {
                REQUIRE(-- (-- it) != begin);
            } THEN("-- (-- it) is not equal to end") {
                REQUIRE(-- (-- it) != end);
            } THEN("-- (-- (-- it)) is equal to begin") {
                REQUIRE(-- (-- (-- it)) == begin);
            } THEN("-- (-- (-- it)) is not equal to end") {
                REQUIRE(-- (-- (-- it)) != end);
            }
        }
        WHEN("Deleting 1st point") {
            auto it_2nd = p.begin().next();
            auto it     = p.begin().remove();
            THEN("Size is 2") {
                REQUIRE(p.size() == 2);
            } THEN("p.begin().remove() == it_2nd") {
                REQUIRE(it == it_2nd);
            } THEN("it_2nd == new begin()") {
                REQUIRE(it_2nd == p.begin());
            }
        }
        WHEN("Deleting 2nd point") {
            auto it_1st = p.begin();
            auto it_2nd = it_1st.next();
            auto it = it_2nd.remove();
            THEN("Size is 2") {
                REQUIRE(p.size() == 2);
                REQUIRE(! p.empty());
            } THEN("it_2nd.remove() == it_3rd") {
                REQUIRE(it == it_2nd);
            } THEN("it_1st == new begin()") {
                REQUIRE(it_1st == p.begin());
            }
        }
        WHEN("Deleting two points") {
            p.begin().remove().remove();
            THEN("Size is 1") {
                REQUIRE(p.size() == 1);
            } THEN("p.begin().next() == p.begin()") {
                REQUIRE(p.begin().next() == p.begin());
            } THEN("p.begin().prev() == p.begin()") {
                REQUIRE(p.begin().prev() == p.begin());
            }
        }
        WHEN("Deleting all points") {
            auto it = p.begin().remove().remove().remove();
            THEN("Size is 0") {
                REQUIRE(p.size() == 0);
                REQUIRE(p.empty());
            } THEN("! p.begin().valid()") {
                REQUIRE(!p.begin().valid());
            } THEN("last iterator not valid") {
                REQUIRE(! it.valid());
            }
        }
        WHEN("Inserting a point at the beginning") {
            p.insert(p.begin(), { 3, 4 });
            THEN("Polygon content is ok") {
                REQUIRE(p == MutablePolygon{ { 0, 0 }, { 0, 1 }, { 1, 0 }, { 3, 4 } });
            }
        }
        WHEN("Inserting a point at the 2nd position") {
            p.insert(++ p.begin(), { 3, 4 });
            THEN("Polygon content is ok") {
                REQUIRE(p == MutablePolygon{ { 0, 0 }, { 3, 4 }, { 0, 1 }, { 1, 0 } });
            }
        } WHEN("Inserting a point after a point was removed") {
            size_t capacity = p.capacity();
            THEN("Initial capacity is 3") {
                REQUIRE(capacity == 3);
            }
            p.begin().remove();
            THEN("After removal of the 1st point the capacity is still 3") {
                REQUIRE(p.capacity() == 3);
            }
            THEN("After removal of the 1st point the content is ok") {
                REQUIRE(p == MutablePolygon{ { 0, 1 }, { 1, 0 } });
            }
            p.insert(p.begin(), { 5, 6 });
            THEN("After insertion at head position the polygon content is ok") {
                REQUIRE(p == MutablePolygon{ { 0, 1 }, { 1, 0 }, { 5, 6 } });
            } THEN("and the capacity is still 3") {
                REQUIRE(p.capacity() == 3);
            }
        }
    }
}

SCENARIO("Remove degenerate points from MutablePolygon", "[MutablePolygon]") {
    GIVEN("Polygon with duplicate points"){
        Slic3r::MutablePolygon p({
            { 0, 0 },
            { 0, 100 }, { 0, 100 }, { 0, 100 },
            { 0, 150 },
            { 0, 200 },
            { 200, 200 },
            { 180, 200 }, { 180, 200 },
            { 180, 20 },
            { 180, 0 },
        });
        WHEN("Duplicate points are removed") {
            remove_duplicates(p);
            THEN("Polygon content is ok") {
                REQUIRE(p == Slic3r::MutablePolygon{ { 0, 0 }, { 0, 100 }, { 0, 150 }, { 0, 200 }, { 200, 200 }, { 180, 200 }, { 180, 20 }, { 180, 0 } });
            }
        }
    }
}

SCENARIO("smooth_outward", "[MutablePolygon]") {
    GIVEN("Convex polygon") {
        MutablePolygon p{ { 0, 0 }, { scaled<coord_t>(10.), 0 }, { 0, scaled<coord_t>(10.) } };
        WHEN("smooth_outward") {
            MutablePolygon p2{ p };
            smooth_outward(p2, scaled<double>(10.));
            THEN("Polygon is unmodified") {
                REQUIRE(p == p2);
            }
        }
    }
    GIVEN("Sharp tiny concave polygon (hole)") {
        MutablePolygon p{ { 0, 0 }, { 0, scaled<coord_t>(5.) }, { scaled<coord_t>(10.), 0 } };
        WHEN("smooth_outward") {
            MutablePolygon p2{ p };
            smooth_outward(p2, scaled<double>(10.));
            THEN("Hole is closed") {
                REQUIRE(p2.empty());
            }
        }
    }
    GIVEN("Two polygons") {
        Polygons p{ { { 0, 0 }, { scaled<coord_t>(10.), 0 }, { 0, scaled<coord_t>(10.) } },
                    { { 0, 0 }, { 0, scaled<coord_t>(5.) }, { scaled<coord_t>(10.), 0 } } };
        WHEN("smooth_outward") {
            p = smooth_outward(p, scaled<double>(10.));
            THEN("CCW contour unmodified, CW contour removed.") {
                REQUIRE(p == Polygons{ { { 0, 0 }, { scaled<coord_t>(10.), 0 }, { 0, scaled<coord_t>(10.) } } });
            }
        }
    }
}
