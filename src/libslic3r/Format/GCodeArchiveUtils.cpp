#include "GCodeArchiveUtils.hpp"

#include "libslic3r/miniz_extension.hpp"
#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <clocale>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <locale>
#include <regex>
#include <sstream>

#include <boost/algorithm/string.hpp>

namespace Slic3r {
namespace GCodeArchiveUtils {

ScopedCNumericLocale::ScopedCNumericLocale()
{
    const char* cur = std::setlocale(LC_NUMERIC, nullptr);
    m_prev = cur ? cur : "C";
    std::setlocale(LC_NUMERIC, "C");
}

ScopedCNumericLocale::~ScopedCNumericLocale()
{
    std::setlocale(LC_NUMERIC, m_prev.c_str());
}

double parse_double_safe(const std::string& s, double fallback)
{
    std::istringstream iss(s);
    iss.imbue(std::locale::classic());
    double v = 0.0;
    iss >> v;
    if (iss.fail()) return fallback;
    return std::isfinite(v) ? v : fallback;
}

int parse_int_safe(const std::string& s, int fallback)
{
    try { size_t p; return std::stoi(s, &p); }
    catch (...) { return fallback; }
}

int hms_to_seconds(const std::string& s)
{
    const std::regex part_re(R"((\d+)\s*([dhms]))", std::regex::icase);
    int total = 0;
    bool found_unit = false;
    for (std::sregex_iterator it(s.begin(), s.end(), part_re), end; it != end; ++it) {
        found_unit = true;
        const int value = parse_int_safe((*it)[1].str(), 0);
        const char unit = static_cast<char>(std::tolower((*it)[2].str()[0]));
        if      (unit == 'd') total += value * 86400;
        else if (unit == 'h') total += value * 3600;
        else if (unit == 'm') total += value * 60;
        else if (unit == 's') total += value;
    }
    if (!found_unit) total = parse_int_safe(s, 0);
    return total;
}

namespace {

std::string base64_decode(const std::string& input)
{
    static constexpr unsigned char kDec[256] = {
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,52,53,54,55,56,57,58,59,60,61,64,64,64,65,64,64,
        64,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
        64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
    };
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (std::isspace(c)) continue;
        if (c == '=') break;
        const unsigned char d = kDec[c];
        if (d >= 64) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

} // anonymous namespace

std::vector<ExtractedThumbnail> extract_gcode_thumbnails(const std::string& gcode_path)
{
    std::vector<ExtractedThumbnail> out;
    std::ifstream gf(gcode_path);
    if (!gf.is_open()) return out;

    const std::regex begin_re(R"(^;\s*thumbnail\s+begin\s+(\d+)x(\d+)\s+\d+)", std::regex::icase);
    const std::regex end_re  (R"(^;\s*thumbnail\s+end)", std::regex::icase);

    bool collecting = false;
    int w = 0, h = 0;
    std::string b64;
    std::string line;

    while (std::getline(gf, line)) {
        std::smatch m;
        if (!collecting && std::regex_search(line, m, begin_re)) {
            collecting = true;
            w = parse_int_safe(m[1].str(), 0);
            h = parse_int_safe(m[2].str(), 0);
            b64.clear();
            continue;
        }
        if (collecting && std::regex_search(line, end_re)) {
            const std::string bytes = base64_decode(b64);
            if (!bytes.empty()) out.push_back({w, h, bytes});
            collecting = false;
            continue;
        }
        if (collecting) {
            std::string s = line;
            boost::algorithm::trim(s);
            if (!s.empty() && s[0] == ';') s.erase(s.begin());
            boost::algorithm::trim(s);
            b64 += s;
        }
    }
    return out;
}

const ExtractedThumbnail* choose_best_thumbnail(const std::vector<ExtractedThumbnail>& thumbs, int w, int h)
{
    const ExtractedThumbnail* best = nullptr;
    for (const auto& t : thumbs) {
        if (t.width == w && t.height == h) return &t;
        if (!best || (t.width * t.height) > (best->width * best->height)) best = &t;
    }
    return best;
}

bool read_printable_area_size_mm(const PrintConfig& config, double& out_x_mm, double& out_y_mm)
{
    const auto* pa_opt = config.option("printable_area");
    if (!pa_opt) return false;
    try {
        const auto* pts = dynamic_cast<const ConfigOptionPoints*>(pa_opt);
        if (!pts || pts->values.size() < 2) return false;
        double xmin = 1e9, xmax = -1e9, ymin = 1e9, ymax = -1e9;
        for (const auto& p : pts->values) {
            xmin = std::min(xmin, p.x()); xmax = std::max(xmax, p.x());
            ymin = std::min(ymin, p.y()); ymax = std::max(ymax, p.y());
        }
        const double W = xmax - xmin;
        const double H = ymax - ymin;
        if (W > 10.0 && H > 10.0) { out_x_mm = W; out_y_mm = H; return true; }
    } catch (...) {}
    return false;
}

std::string build_iso_date_today()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[16] = {0};
    // %Y-%m-%d only ever produces ASCII digits and '-', independent of locale
    // (unlike e.g. %b, which would print a localized month name).
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
    return std::string(buf);
}

double extrusion_mass_g(double extrusion_mm, double filament_diameter_mm, double density_g_cm3)
{
    const double r = (filament_diameter_mm / 2.0) / 10.0; // cm
    const double l = extrusion_mm / 10.0;                  // cm
    return l * 3.14159265358979323846 * r * r * density_g_cm3;
}

bool write_zip_archive(const std::string& out_path,
                        const std::vector<std::pair<std::string, std::string>>& entries)
{
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, out_path.c_str(), 0)) return false;
    for (const auto& e : entries)
        if (!mz_zip_writer_add_mem(&zip, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_COMPRESSION))
        { mz_zip_writer_end(&zip); return false; }
    const bool ok = mz_zip_writer_finalize_archive(&zip) != 0;
    mz_zip_writer_end(&zip);
    return ok;
}

} // namespace GCodeArchiveUtils
} // namespace Slic3r
