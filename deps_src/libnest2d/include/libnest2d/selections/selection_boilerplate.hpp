#ifndef SELECTION_BOILERPLATE_HPP
#define SELECTION_BOILERPLATE_HPP

#include <atomic>
#include <libnest2d/nester.hpp>

namespace libnest2d { namespace selections {

template<class RawShape>
class SelectionBoilerplate {
public:
    using ShapeType = RawShape;
    using Item = _Item<RawShape>;
    using ItemGroup = _ItemGroup<RawShape>;
    using PackGroup = _PackGroup<RawShape>;

    inline const PackGroup& getResult() const {
        return packed_bins_;
    }

    inline int lastPackedBinId() const { return last_packed_bin_id_; }

    inline void progressIndicator(ProgressFunction fn) { progress_ = fn; }

    inline void stopCondition(StopCondition cond) { stopcond_ = cond; }

    inline void unfitIndicator(UnfitIndicator fn) { unfitindicator_ = fn; }

    inline void clear() { packed_bins_.clear(); }

protected:

    template<class Placer, class Container, class Bin, class PCfg>
    void remove_unpackable_items(Container &c, const Bin &bin, const PCfg& pcfg)
    {
        // Safety test: try to pack each item into an empty bin. If it fails
        // then it should be removed from the list
        Placer p{ bin };
        p.configure(pcfg);
        p.preload(pcfg.m_excluded_items);
        auto it = c.begin();
        while (it != c.end() && !stopcond_()) {

            // WARNING: The copy of itm needs to be created before Placer.
            // Placer is working with references and its destructor still
            // manipulates the item this is why the order of stack creation
            // matters here.
            const Item& itm = *it;
            Item cpy{itm};

            auto result = p.pack(cpy);
            if (itm.area() <= 0 || !result) {
                static_cast<Item&>(*it).binId(BIN_ID_UNFIT);
            }
            it++;
        }
    }

    PackGroup packed_bins_;
    ProgressFunction progress_ = [](unsigned){};
    StopCondition stopcond_ = []() { return false; };
    UnfitIndicator unfitindicator_;
    int last_packed_bin_id_ = -1;
};

}
}

#endif // SELECTION_BOILERPLATE_HPP
