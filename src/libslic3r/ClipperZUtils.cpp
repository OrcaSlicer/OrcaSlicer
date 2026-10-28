#include "ClipperZUtils.hpp"
#include "ClipperUtils.hpp"

#include <clipper2/clipper2_z.hpp>
#include <cstddef>
#include "Point.hpp"

namespace Slic3r {
namespace ClipperZUtils {

namespace C2 = Clipper2Lib_Z;

static C2::Paths64 to_paths64(const ZPaths &paths)
{
    C2::Paths64 out;
    out.reserve(paths.size());
    for (const ZPath &path : paths) {
        C2::Path64 &dst = out.emplace_back();
        dst.reserve(path.size());
        for (const ZPoint &pt : path)
            dst.emplace_back(pt.x(), pt.y(), pt.z());
    }
    return out;
}

static void append_zpaths(const C2::Paths64 &paths, ZPaths &out)
{
    out.reserve(out.size() + paths.size());
    for (const C2::Path64 &path : paths) {
        ZPath &dst = out.emplace_back();
        dst.reserve(path.size());
        for (const C2::Point64 &pt : path)
            dst.emplace_back(pt.x, pt.y, pt.z);
    }
}

double area(const ZPath &path)
{
    const size_t size = path.size();
    if (size < 3)
        return 0.;
    double a = 0.;
    for (size_t i = 0, j = size - 1; i < size; j = i ++)
        a += (double(path[j].x()) + path[i].x()) * (double(path[j].y()) - path[i].y());
    return -a * 0.5;
}

ZPaths clip_zpaths(ClipType clip_type, const ZPaths &subject, bool subject_open, const ZPaths &clip, const ZFillCallback &zfill, bool preserve_collinear)
{
    C2::Clipper64 clipper;
    clipper.PreserveCollinear(preserve_collinear);
    if (zfill)
        clipper.SetZCallback([&zfill](const C2::Point64 &e1bot, const C2::Point64 &e1top, const C2::Point64 &e2bot, const C2::Point64 &e2top, C2::Point64 &pt) {
            // Clipper2 already copied Z from a coincident input vertex.
            auto coincident = [&pt](const C2::Point64 &p) { return p.x == pt.x && p.y == pt.y; };
            if (coincident(e1bot) || coincident(e1top) || coincident(e2bot) || coincident(e2top))
                return;
            ZPoint zpt(pt.x, pt.y, pt.z);
            zfill(ZPoint(e1bot.x, e1bot.y, e1bot.z), ZPoint(e1top.x, e1top.y, e1top.z),
                  ZPoint(e2bot.x, e2bot.y, e2bot.z), ZPoint(e2top.x, e2top.y, e2top.z), zpt);
            pt.z = zpt.z();
        });
    if (subject_open)
        clipper.AddOpenSubject(to_paths64(subject));
    else
        clipper.AddSubject(to_paths64(subject));
    clipper.AddClip(to_paths64(clip));
    const C2::ClipType type = clip_type == ctIntersection ? C2::ClipType::Intersection :
                              clip_type == ctUnion        ? C2::ClipType::Union :
                              clip_type == ctDifference   ? C2::ClipType::Difference : C2::ClipType::Xor;
    C2::Paths64 closed, open;
    clipper.Execute(type, C2::FillRule::NonZero, closed, open);
    ZPaths out;
    append_zpaths(closed, out);
    append_zpaths(open, out);
    return out;
}

} // namespace ClipperZUtils
} // namespace Slic3r
