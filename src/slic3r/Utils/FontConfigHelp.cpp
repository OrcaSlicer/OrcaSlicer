#include "FontConfigHelp.hpp"

#ifdef EXIST_FONT_CONFIG_INCLUDE

#include <wx/filename.h>
#include <fontconfig/fontconfig.h>
#include "libslic3r/Utils.hpp"

using namespace Slic3r::GUI;


// @Vojta suggest to make static variable global
// Guard for finalize Font Config
// Will be finalized on application exit
// It seams that it NOT work
static std::optional<Slic3r::ScopeGuard> finalize_guard;
// cache for Loading of the default configuration file and building information about the available fonts.
static FcConfig *fc = nullptr;

std::string Slic3r::GUI::get_font_path(const wxFont &font, bool reload_fonts)
{
    if (!finalize_guard.has_value()) {
        FcInit();
        fc = FcInitLoadConfigAndFonts();
        finalize_guard.emplace([]() {
            // Some internal problem of Font config or other library use FC too(like wxWidget)
            // fccache.c:795: FcCacheFini: Assertion `fcCacheChains[i] == NULL' failed. 
            //FcFini(); 
            FcConfigDestroy(fc);
        });
    } else if (reload_fonts) {
        FcConfigDestroy(fc);
        fc = FcInitLoadConfigAndFonts();
    }

    if (fc == nullptr) return "";

    wxString                 fontDesc = font.GetNativeFontInfoUserDesc();
    wxString                 faceName = font.GetFaceName();
    const wxScopedCharBuffer faceNameBuffer = faceName.ToUTF8();
    const char *             fontFamily     = faceNameBuffer;

    // Check font slant
    int slant = FC_SLANT_ROMAN;
    if (fontDesc.Find(wxS("Oblique")) != wxNOT_FOUND)
        slant = FC_SLANT_OBLIQUE;
    else if (fontDesc.Find(wxS("Italic")) != wxNOT_FOUND)
        slant = FC_SLANT_ITALIC;

    // Check font weight
    int weight = FC_WEIGHT_NORMAL;
    if (fontDesc.Find(wxS("Book")) != wxNOT_FOUND)
        weight = FC_WEIGHT_BOOK;
    else if (fontDesc.Find(wxS("Medium")) != wxNOT_FOUND)
        weight = FC_WEIGHT_MEDIUM;
#ifdef FC_WEIGHT_ULTRALIGHT
    else if (fontDesc.Find(wxS("Ultra-Light")) != wxNOT_FOUND)
        weight = FC_WEIGHT_ULTRALIGHT;
#endif
    else if (fontDesc.Find(wxS("Light")) != wxNOT_FOUND)
        weight = FC_WEIGHT_LIGHT;
    else if (fontDesc.Find(wxS("Semi-Bold")) != wxNOT_FOUND)
        weight = FC_WEIGHT_DEMIBOLD;
#ifdef FC_WEIGHT_ULTRABOLD
    else if (fontDesc.Find(wxS("Ultra-Bold")) != wxNOT_FOUND)
        weight = FC_WEIGHT_ULTRABOLD;
#endif
    else if (fontDesc.Find(wxS("Bold")) != wxNOT_FOUND)
        weight = FC_WEIGHT_BOLD;
    else if (fontDesc.Find(wxS("Heavy")) != wxNOT_FOUND)
        weight = FC_WEIGHT_BLACK;

    // Check font width
    int width = FC_WIDTH_NORMAL;
    if (fontDesc.Find(wxS("Ultra-Condensed")) != wxNOT_FOUND)
        width = FC_WIDTH_ULTRACONDENSED;
    else if (fontDesc.Find(wxS("Extra-Condensed")) != wxNOT_FOUND)
        width = FC_WIDTH_EXTRACONDENSED;
    else if (fontDesc.Find(wxS("Semi-Condensed")) != wxNOT_FOUND)
        width = FC_WIDTH_SEMICONDENSED;
    else if (fontDesc.Find(wxS("Condensed")) != wxNOT_FOUND)
        width = FC_WIDTH_CONDENSED;
    else if (fontDesc.Find(wxS("Ultra-Expanded")) != wxNOT_FOUND)
        width = FC_WIDTH_ULTRAEXPANDED;
    else if (fontDesc.Find(wxS("Extra-Expanded")) != wxNOT_FOUND)
        width = FC_WIDTH_EXTRAEXPANDED;
    else if (fontDesc.Find(wxS("Semi-Expanded")) != wxNOT_FOUND)
        width = FC_WIDTH_SEMIEXPANDED;
    else if (fontDesc.Find(wxS("Expanded")) != wxNOT_FOUND)
        width = FC_WIDTH_EXPANDED;

    FcResult   res;
    FcPattern *matchPattern = FcPatternBuild(NULL, FC_FAMILY, FcTypeString,
                                             (FcChar8 *) fontFamily, NULL);
    ScopeGuard sg_mp([matchPattern]() { FcPatternDestroy(matchPattern); });

    FcPatternAddInteger(matchPattern, FC_SLANT, slant);
    FcPatternAddInteger(matchPattern, FC_WEIGHT, weight);
    FcPatternAddInteger(matchPattern, FC_WIDTH, width);

    FcConfigSubstitute(NULL, matchPattern, FcMatchPattern);
    FcDefaultSubstitute(matchPattern);

    FcPattern *resultPattern = FcFontMatch(NULL, matchPattern, &res);
    if (resultPattern == nullptr) return "";
    ScopeGuard sg_rp([resultPattern]() { FcPatternDestroy(resultPattern); });

    FcChar8 *fileName;
    if (FcPatternGetString(resultPattern, FC_FILE, 0, &fileName) !=
        FcResultMatch)
        return "";
    wxString fontFileName = wxString::FromUTF8((char *) fileName);

    if (fontFileName.IsEmpty()) return "";

    // find full file path
    wxFileName myFileName(fontFileName);
    if (!myFileName.IsOk()) return "";

    if (myFileName.IsRelative()) {
        // Check whether the file is relative to the current working directory
        if (!(myFileName.MakeAbsolute() && myFileName.FileExists())) {
            return "";
            // File not found, search in given search paths
            // wxString foundFileName =
            // m_searchPaths.FindAbsoluteValidPath(fileName); if
            // (!foundFileName.IsEmpty()) {
            //    myFileName.Assign(foundFileName);
            //}
        }
    }

    if (!myFileName.FileExists() || !myFileName.IsFileReadable()) return "";

    // File exists and is accessible
    wxString fullFileName = myFileName.GetFullPath();
    return std::string(fullFileName.c_str());
}

#endif // EXIST_FONT_CONFIG_INCLUDE