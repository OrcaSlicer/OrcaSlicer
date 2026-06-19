#include <catch2/catch_all.hpp>

#include "libnest2d_test_utils.hpp"

using namespace libnest2d;

// NfpPlacer is the No-Fit-Polygon placement engine that Orca's arranger drives
// (via _Nester/FirstFitSelection in Arrange.cpp). These exercise the placer
// directly: pack()/accept() are the core geometric placement primitives.
namespace {

struct NfpPlacerFixture {
    using Cfg = NfpPlacer::Config;
    Box bin{250000000, 210000000};   // 250 x 210 mm bed at 1e6 scale

    NfpPlacer placer_with(Cfg cfg = {}) const {
        cfg.parallel = false;        // deterministic, single-threaded for tests
        NfpPlacer p{bin};
        p.configure(cfg);
        return p;
    }

    // pack + accept; returns whether the item was placed.
    static bool place(NfpPlacer &p, Item &item) {
        auto res = p.pack(item);
        if (res) p.accept(res);
        return bool(res);
    }

    // Place every item and REQUIRE each one is packed.
    static void place_all(NfpPlacer &p, std::vector<RectangleItem> &items) {
        for (size_t i = 0; i < items.size(); ++i) {
            INFO("packing item " << i);
            REQUIRE(place(p, items[i]));
        }
    }

    // No two items overlap (a shared edge is allowed) and each stays in the bin.
    void require_disjoint_in_bin(std::vector<RectangleItem> &items) const {
        for (size_t i = 0; i < items.size(); ++i) {
            REQUIRE(sl::isInside(items[i].boundingBox(), bin));
            for (size_t j = i + 1; j < items.size(); ++j) {
                const bool overlaps = Item::intersects(items[i], items[j]) &&
                                      !Item::touches(items[i], items[j]);
                INFO("items " << i << " and " << j);
                REQUIRE_FALSE(overlaps);
            }
        }
    }

    static std::vector<RectangleItem> squares(size_t n, Coord side) {
        return std::vector<RectangleItem>(n, RectangleItem{side, side});
    }
};

} // namespace

TEST_CASE_METHOD(NfpPlacerFixture, "NfpPlacer places a single item inside the bin", "[Nesting][Placer]") {
    NfpPlacer placer = placer_with();
    RectangleItem item{100000000, 100000000};

    REQUIRE(place(placer, item));
    REQUIRE(placer.getItems().size() == 1u);
    REQUIRE(sl::isInside(item.boundingBox(), bin));
}

TEST_CASE_METHOD(NfpPlacerFixture, "NfpPlacer rejects an item larger than the bin", "[Nesting][Placer]") {
    NfpPlacer placer = placer_with();
    RectangleItem big{300000000, 300000000};   // wider and taller than the bin

    auto res = placer.pack(big);
    REQUIRE_FALSE(bool(res));
    REQUIRE(placer.getItems().empty());
}

TEST_CASE_METHOD(NfpPlacerFixture, "NfpPlacer positions the first item for any starting point", "[Nesting][Placer]") {
    // setInitialPosition() seeds the first item from the configured starting
    // corner; pack() (without accept()) drives that switch for every value.
    using A = Cfg::Alignment;
    auto start = GENERATE(A::CENTER, A::BOTTOM_LEFT, A::BOTTOM_RIGHT,
                          A::TOP_LEFT, A::TOP_RIGHT, A::USER_DEFINED, A::DONT_ALIGN);
    CAPTURE(int(start));

    Cfg cfg;
    cfg.starting_point  = start;
    cfg.best_object_pos = bin.center();
    NfpPlacer placer = placer_with(cfg);

    RectangleItem item{100000000, 100000000};
    auto res = placer.pack(item);
    REQUIRE(bool(res));
    REQUIRE(sl::isInside(item.boundingBox(), bin));
}

TEST_CASE_METHOD(NfpPlacerFixture, "NfpPlacer packs many items without overlap", "[Nesting][Placer]") {
    // Each item is placed against the no-fit polygon of the growing pile.
    auto items = squares(GENERATE(2u, 6u, 9u), 60000000);
    NfpPlacer placer = placer_with();

    place_all(placer, items);
    REQUIRE(placer.getItems().size() == items.size());
    require_disjoint_in_bin(items);
}

TEST_CASE_METHOD(NfpPlacerFixture, "NfpPlacer evaluates the rotation candidates", "[Nesting][Placer]") {
    Cfg cfg;
    cfg.rotations = {0.0, Pi / 2.0};        // exercise the rotation search loop
    NfpPlacer placer = placer_with(cfg);

    std::vector<RectangleItem> rects = {
        {180000000, 40000000}, {180000000, 40000000}, {180000000, 40000000}};
    place_all(placer, rects);
    require_disjoint_in_bin(rects);
}

TEST_CASE_METHOD(NfpPlacerFixture, "NfpPlacer's final alignment keeps the pile clear of a fixed obstacle", "[Nesting][Placer]") {
    // A preloaded fixed item makes finalAlign's recentring keep the pile clear of
    // it instead of dropping it straight onto the bin centre. Box{w,h} centres on
    // the origin, so the obstacle sits there too; virtual keeps it in place.
    RectangleItem obstacle{80000000, 80000000};
    obstacle.translation({-40000000, -40000000});    // 80x80 mm centred in the bin (origin)
    obstacle.markAsFixedInBin(0);
    obstacle.is_virt_object = true;

    auto items = squares(4, 30000000);
    {
        NfpPlacer placer = placer_with();
        NfpPlacer::ItemGroup fixed;
        fixed.emplace_back(obstacle);
        placer.preload(fixed);
        place_all(placer, items);
    } // the placer's destructor runs finalAlign, translating the packed items

    for (size_t i = 0; i < items.size(); ++i) {
        INFO("item " << i);
        const bool overlaps = Item::intersects(items[i], obstacle) &&
                              !Item::touches(items[i], obstacle);
        REQUIRE_FALSE(overlaps);
    }
}
