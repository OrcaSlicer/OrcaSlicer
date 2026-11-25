// FontConfigHelp lives in libslic3r_gui; this is the only suite that links it.
#ifdef __linux__

#include <catch2/catch_test_macros.hpp>
#include <fontconfig/fontconfig.h>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "test_utils.hpp"
#include "slic3r/Utils/FontConfigHelp.hpp"

namespace {

namespace fs = boost::filesystem;

// 2020-01-01, stamped on the font directories so the caches never look stale.
const std::time_t scanned_at = 1577836800;

int current_font_count()
{
    FcConfig *config = FcConfigGetCurrent();
    if (config == nullptr)
        return 0;
    const FcFontSet *fonts = FcConfigGetFonts(config, FcSetSystem);
    return fonts == nullptr ? 0 : fonts->nfont;
}

int cache_file_count(const fs::path &dir)
{
    int count = 0;
    for (const fs::directory_entry &entry : fs::directory_iterator(dir))
        if (entry.path().filename().string().find(".cache-") != std::string::npos)
            ++count;
    return count;
}

// The temp directory's parent comes from TMPDIR, which can contain XML metacharacters.
std::string xml_escaped(const std::string &text)
{
    std::string escaped;
    for (const char c : text)
        switch (c) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default: escaped += c; break;
        }
    return escaped;
}

// A Flatpak reads the host's cache before its own and can only write its own.
enum class CacheDirs { OwnOnly, HostThenOwn };

// The font is one level below the configured directory, as in a real font tree.
struct FontTree
{
    ScopedTemporaryDir root{"orca-fontcache"};
    fs::path           top_dir     = root.path() / "fonts";
    fs::path           nested_dir  = top_dir / "truetype";
    fs::path           host_cache  = root.path() / "host-cache";
    fs::path           own_cache   = root.path() / "own-cache";
    fs::path           config_file = root.path() / "fonts.conf";

    explicit FontTree(CacheDirs cache_dirs)
    {
        if (cache_dirs == CacheDirs::HostThenOwn)
            m_listed_caches.push_back(host_cache);
        m_listed_caches.push_back(own_cache);

        fs::create_directories(nested_dir);
        for (const fs::path &dir : m_listed_caches)
            fs::create_directories(dir);

        std::ofstream out(config_file.string());
        out << "<?xml version=\"1.0\"?>\n<fontconfig>\n"
            << "  <dir>" << xml_escaped(top_dir.string()) << "</dir>\n";
        for (const fs::path &dir : m_listed_caches)
            out << "  <cachedir>" << xml_escaped(dir.string()) << "</cachedir>\n";
        out << "</fontconfig>\n";
    }

    // Fontconfig writes to the first writable cache directory the configuration
    // lists, and the constructor makes all of them writable.
    const fs::path &damaged_cache() const { return m_listed_caches.front(); }

    FontTree(const FontTree &)            = delete;
    FontTree &operator=(const FontTree &) = delete;

    void add_font()
    {
        const std::string font = shipped_resource("fonts/HarmonyOS_Sans_SC_Regular.ttf");
        REQUIRE(fs::exists(font));
        fs::copy_file(font, nested_dir / "font.ttf");
    }

    // The caches stay valid while reporting an empty tree.
    void poison()
    {
        set_times();
        FcConfig *config = FcConfigCreate();
        REQUIRE(config != nullptr);
        REQUIRE(FcConfigParseAndLoad(config, reinterpret_cast<const FcChar8 *>(config_file.c_str()), FcTrue) == FcTrue);
        for (const fs::path &dir : {top_dir, nested_dir}) {
            FcCache *cache = FcDirCacheRead(reinterpret_cast<const FcChar8 *>(dir.c_str()), FcTrue, config);
            REQUIRE(cache != nullptr);
            FcDirCacheUnload(cache);
        }
        FcConfigDestroy(config);
        REQUIRE(cache_file_count(damaged_cache()) > 0);

        add_font();
        set_times();
    }

private:
    void set_times()
    {
        fs::last_write_time(nested_dir, scanned_at);
        fs::last_write_time(top_dir, scanned_at);
    }

    std::vector<fs::path> m_listed_caches;
};

// fontconfig's current configuration is process-wide, as is FONTCONFIG_FILE.
// Anything in this binary holding a configuration of its own, such as the one
// get_font_path caches, still resolves against whichever one is current.
struct ScopedFontConfig
{
    std::string previous;
    bool        had_previous = false;

    explicit ScopedFontConfig(const fs::path &config_file)
    {
        if (const char *current = ::getenv("FONTCONFIG_FILE")) {
            previous     = current;
            had_previous = true;
        }
        ::setenv("FONTCONFIG_FILE", config_file.c_str(), 1);
        FcInitReinitialize();
    }
    ~ScopedFontConfig()
    {
        if (had_previous)
            ::setenv("FONTCONFIG_FILE", previous.c_str(), 1);
        else
            ::unsetenv("FONTCONFIG_FILE");
        FcInitReinitialize();
    }
    ScopedFontConfig(const ScopedFontConfig &)            = delete;
    ScopedFontConfig &operator=(const ScopedFontConfig &) = delete;
};

} // namespace

TEST_CASE("A font cache that reports no font is rebuilt", "[FontConfigHelp][Regression]")
{
    FontTree tree(CacheDirs::OwnOnly);
    tree.poison();

    ScopedFontConfig scoped(tree.config_file);
    REQUIRE(current_font_count() == 0);

    Slic3r::GUI::repair_unusable_font_cache();
    CHECK(current_font_count() > 0);
}

// Fontconfig writes to a cache directory whose write bits are cleared, so this
// cannot reproduce a Flatpak's read-only mount of the host cache, and which
// directory receives the rebuilt caches is not asserted.
TEST_CASE("A font cache damaged in another cache directory is repaired", "[FontConfigHelp][Regression]")
{
    FontTree tree(CacheDirs::HostThenOwn);
    tree.poison();
    REQUIRE(cache_file_count(tree.own_cache) == 0);

    ScopedFontConfig scoped(tree.config_file);
    REQUIRE(current_font_count() == 0);

    Slic3r::GUI::repair_unusable_font_cache();
    CHECK(current_font_count() > 0);
}

TEST_CASE("A usable font cache is not rebuilt", "[FontConfigHelp]")
{
    FontTree tree(CacheDirs::OwnOnly);
    tree.add_font();

    ScopedFontConfig scoped(tree.config_file);
    const int before = current_font_count();
    REQUIRE(before > 0);

    Slic3r::GUI::repair_unusable_font_cache();
    CHECK(current_font_count() == before);
}

#endif // __linux__
