#ifndef SVGTOOLS_HPP
#define SVGTOOLS_HPP

#include <iostream>
#include <fstream>
#include <string>

#include <boost/filesystem.hpp>
#include <libnest2d/nester.hpp>

namespace libnest2d { namespace svg {

template<class RawShape>
class SVGWriter {
    using Item = _Item<RawShape>;
    using Coord = TCoord<TPoint<RawShape>>;
    using Box = _Box<TPoint<RawShape>>;
    using PackGroup = _PackGroup<RawShape>;

public:

    enum OrigoLocation {
        TOPLEFT,
        BOTTOMLEFT
    };

    struct Config {
        OrigoLocation origo_location;
        Coord mm_in_coord_units;
        double width, height;
        double        x0, y0;
        Config():
            origo_location(BOTTOMLEFT), mm_in_coord_units(1000000),
            width(500), height(500),x0(100) {}

    };

    Config conf_;

private:
    std::vector<std::string> svg_layers_;
    bool finished_ = false;
public:

    SVGWriter(const Config& conf = Config()):
        conf_(conf) {}

    void setSize(const Box &box)    {
        conf_.x0     = box.width() / 5;
        conf_.x0     = box.height() / 5;
        conf_.height = static_cast<double>(box.height() + conf_.y0*2) /
                conf_.mm_in_coord_units;
        conf_.width  = static_cast<double>(box.width() + conf_.x0*2) /
                conf_.mm_in_coord_units;
    }
    
    void writeShape(RawShape tsh, std::string fill = "none", std::string stroke = "black", float stroke_width = 1) {
        if(svg_layers_.empty()) addLayer();
        if(conf_.origo_location == BOTTOMLEFT) {
            auto d = static_cast<Coord>(
                std::round(conf_.height*conf_.mm_in_coord_units) );

            auto& contour = shapelike::contour(tsh);
            for (auto &v : contour) {
                setX(v, getX(v) + conf_.x0); // right shift so we can draw outside the bounding box
                setY(v, -getY(v) + d + conf_.y0);
            }

            auto& holes = shapelike::holes(tsh);
            for (auto &h : holes)
                for (auto &v : h) {
                    setX(v, getX(v) + conf_.x0);
                    setY(v, -getY(v) + d + conf_.y0);
                }
        }
        currentLayer() +=
            shapelike::serialize<Formats::SVG>(tsh,
                                               1.0 / conf_.mm_in_coord_units, fill, stroke, stroke_width) +
            "\n";
    }

    void writeItem(const Item& item, std::string fill = "none", std::string stroke = "black", float stroke_width = 1) {
        writeShape(item.transformedShape(), fill, stroke, stroke_width);
    }

    void writePackGroup(const PackGroup& result) {
        for(auto r : result) {
            addLayer();
            for(Item& sh : r) {
                writeItem(sh);
            }
            finishLayer();
        }
    }
    
    template<class ItemIt> void writeItems(ItemIt from, ItemIt to) {
        auto it = from;
        PackGroup pg;
        while(it != to) {
            if(it->binId() == BIN_ID_UNSET) continue;
            while(pg.size() <= size_t(it->binId())) pg.emplace_back();
            pg[it->binId()].emplace_back(*it);
            ++it;
        }
        writePackGroup(pg);
    }

    void draw_text(float x,float y, const std::string text, const std::string color, int font_size)
    {
        char s[500];
        sprintf(s,
            "<text x=\"%f\" y=\"%f\" font-family=\"sans-serif\" font-size=\"%dpx\" fill=\"%s\">%s</text>\n",
            x,y, font_size, color.c_str(), text.c_str());
        currentLayer() += s;
    }

    void addLayer() {
        svg_layers_.emplace_back(header());
        finished_ = false;
    }

    void finishLayer() {
        currentLayer() += "\n</svg>\n";
        finished_ = true;
    }

    void save(const std::string& filepath) {
        size_t lyrc = svg_layers_.size() > 1? 1 : 0;
        size_t last = svg_layers_.size() > 1? svg_layers_.size() : 0;

        for(auto& lyr : svg_layers_) {
            std::fstream out(filepath + (lyrc > 0? std::to_string(lyrc) : "") +
                             ".svg", std::fstream::out);
            if(out.is_open()) out << lyr;
            if(lyrc == last && !finished_) out << "\n</svg>\n";
            out.flush(); out.close(); lyrc++;
        };
    }

    // save svg in utf-8 file name
    void save(const boost::filesystem::path &filepath)
    {
        size_t lyrc = svg_layers_.size() > 1 ? 1 : 0;
        size_t last = svg_layers_.size() > 1 ? svg_layers_.size() : 0;

        for (auto &lyr : svg_layers_) {
            std::ofstream out(filepath.string(), std::fstream::out);
            if (out.is_open()) out << lyr;
            if (lyrc == last && !finished_) out << "\n</svg>\n";
            out.flush();
            out.close();
            lyrc++;
        };
    }

private:

    std::string& currentLayer() { return svg_layers_.back(); }

    const std::string header() const {
        std::string svg_header =
R"raw(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.0//EN" "http://www.w3.org/TR/2001/REC-SVG-20010904/DTD/svg10.dtd">
<svg height=")raw";
        svg_header += std::to_string(conf_.height) + "\" width=\"" + std::to_string(conf_.width) + "\" ";
        svg_header += R"raw(xmlns="http://www.w3.org/2000/svg" xmlns:svg="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">)raw";
        return svg_header;
    }

};

}
}

#endif // SVGTOOLS_HPP
