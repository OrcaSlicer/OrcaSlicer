#ifndef PRINTER_PARTS_H
#define PRINTER_PARTS_H

#include <vector>
#include <libnest2d/libnest2d.hpp>

using TestData = std::vector<libnest2d::PathImpl>;
using TestDataEx = std::vector<libnest2d::PolygonImpl>;

extern const TestData PRINTER_PART_POLYGONS;
extern const TestData STEGOSAUR_POLYGONS;
extern const TestDataEx PRINTER_PART_POLYGONS_EX;

#endif // PRINTER_PARTS_H
