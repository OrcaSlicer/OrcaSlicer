#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"

#include "TextShape.hpp"

#include <string>
#include <vector>

#include "Standard_TypeDef.hxx"
#include "STEPCAFControl_Reader.hxx"
#include "BRepMesh_IncrementalMesh.hxx"
#include "Interface_Static.hxx"
#include "XCAFDoc_DocumentTool.hxx"
#include "XCAFDoc_ShapeTool.hxx"
#include "XCAFApp_Application.hxx"
#include "TopoDS_Solid.hxx"
#include "TopoDS_Compound.hxx"
#include "TopoDS_Builder.hxx"
#include "TopoDS.hxx"
#include "TDataStd_Name.hxx"
#include "BRepBuilderAPI_Transform.hxx"
#include "TopExp_Explorer.hxx"
#include "TopExp_Explorer.hxx"
#include "BRep_Tool.hxx"
#include "Font_BRepFont.hxx"
#include "Font_BRepTextBuilder.hxx"
#include "BRepPrimAPI_MakePrism.hxx"
#include "Font_FontMgr.hxx"

#include <boost/log/trivial.hpp>

namespace Slic3r {

static std::map<std::string, std::string> g_occt_fonts_maps; //map<font_name, font_path>

static const std::vector<Standard_CString> fonts_suffix{ "Bold",  "Medium", "Heavy", "Italic", "Oblique", "Inclined", "Light", "Thin", 
"Semibold", "ExtraBold", "ExtraBold",  "Semilight", "SemiLight", "ExtraLight", "Extralight",  "Ultralight", 
"Condensed", "Ultra", "Extra", "Expanded", "Extended", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Al Tarikh"};

std::map<std::string, std::string> get_occt_fonts_maps()
{
    return g_occt_fonts_maps;
}

std::vector<std::string> init_occt_fonts()
{
    std::vector<std::string> stdFontNames;

    Handle(Font_FontMgr) aFontMgr = Font_FontMgr::GetInstance();
    aFontMgr->InitFontDataBase();

    TColStd_SequenceOfHAsciiString availFontNames;
    aFontMgr->GetAvailableFontsNames(availFontNames);
    stdFontNames.reserve(availFontNames.Size());

    g_occt_fonts_maps.clear();

    BOOST_LOG_TRIVIAL(info) << "init_occt_fonts start";
#ifdef __APPLE__
    //from resource
    stdFontNames.push_back("HarmonyOS Sans SC");
    g_occt_fonts_maps.insert(std::make_pair("HarmoneyOS Sans SC", Slic3r::resources_dir() + "/fonts/" + "HarmonyOS_Sans_SC_Regular.ttf"));
#endif
    for (auto afn : availFontNames) {
#ifdef __APPLE__
        if(afn->String().StartsWith("."))
            continue;
#endif
        if(afn->Search("Emoji") != -1 || afn->Search("emoji") != -1)
            continue;
        bool repeat = false;
        for (size_t i = 0; i < fonts_suffix.size(); i++) {
            if (afn->SearchFromEnd(fonts_suffix[i]) != -1) {
                repeat = true;
                break;
            }
        }
        if (repeat)
            continue;

        Handle(Font_SystemFont) sys_font = aFontMgr->GetFont(afn->ToCString());
        TCollection_AsciiString font_path = sys_font->FontPath(Font_FontAspect::Font_FontAspect_Regular);
        if (!font_path.IsEmpty() && font_path.SearchFromEnd(".") != -1) {
            auto  file_type = font_path.SubString(font_path.SearchFromEnd(".") + 1, font_path.Length());
            file_type.LowerCase();
            if (file_type == "ttf" || file_type == "otf" || file_type == "ttc") {
                g_occt_fonts_maps.insert(std::make_pair(afn->ToCString(), decode_path(font_path.ToCString())));
            }
        }
    }
    BOOST_LOG_TRIVIAL(info) << "init_occt_fonts end";
    // in order
    for (auto occt_font : g_occt_fonts_maps) {
        stdFontNames.push_back(occt_font.first);
    }
    return stdFontNames;
}

static bool TextToBRep(const char* text, const char* font, const float theTextHeight, Font_FontAspect& theFontAspect, TopoDS_Shape& theShape, double& text_width)
{
    Standard_Integer anArgIt = 1;
    Standard_CString aName = "text_shape";
    Standard_CString aText = text;

    Font_BRepFont           aFont;
    //TCollection_AsciiString aFontName("Courier");
    TCollection_AsciiString aFontName(font);
    Standard_Real           aTextHeight = theTextHeight;
    Font_FontAspect         aFontAspect = theFontAspect;
    Standard_Boolean        anIsCompositeCurve = Standard_False;
    gp_Ax3                  aPenAx3(gp::XOY());
    gp_Dir                  aNormal(0.0, 0.0, 1.0);
    gp_Dir                  aDirection(1.0, 0.0, 0.0);
    gp_Pnt                  aPenLoc;

    Graphic3d_HorizontalTextAlignment aHJustification = Graphic3d_HTA_LEFT;
    Graphic3d_VerticalTextAlignment   aVJustification = Graphic3d_VTA_BOTTOM;
    Font_StrictLevel aStrictLevel = Font_StrictLevel_Any;

    aFont.SetCompositeCurveMode(anIsCompositeCurve);
    if (!aFont.FindAndInit(aFontName.ToCString(), aFontAspect, aTextHeight, aStrictLevel))
        return false;

    aPenAx3 = gp_Ax3(aPenLoc, aNormal, aDirection);

    Handle(Font_TextFormatter) aFormatter = new Font_TextFormatter();
    aFormatter->Reset();
    aFormatter->SetupAlignment(aHJustification, aVJustification);
    aFormatter->Append(aText, *aFont.FTFont());
    aFormatter->Format();

    // get the text width
    text_width                  = 0;
    NCollection_String coll_str = aText;
    for (NCollection_Utf8Iter anIter = coll_str.Iterator(); *anIter != 0;) {
        const Standard_Utf32Char aCharThis = *anIter;
        const Standard_Utf32Char aCharNext = *++anIter;
        double                   width     = aFont.AdvanceX(aCharThis, aCharNext);
        text_width += width;
    }

    Font_BRepTextBuilder aBuilder;
    theShape = aBuilder.Perform(aFont, aFormatter, aPenAx3);
    return true;
}

static bool Prism(const TopoDS_Shape& theBase, const float thickness, TopoDS_Shape& theSolid)
{
    if (theBase.IsNull()) return false;

    gp_Vec V(0.f, 0.f, thickness);
    BRepPrimAPI_MakePrism* Prism = new BRepPrimAPI_MakePrism(theBase, V, Standard_False);

    theSolid = Prism->Shape();
    return true;
}

static void MakeMesh(TopoDS_Shape& theSolid, TriangleMesh& theMesh)
{
    const double STEP_TRANS_CHORD_ERROR = 0.005;
    const double STEP_TRANS_ANGLE_RES = 1;

    BRepMesh_IncrementalMesh mesh(theSolid, STEP_TRANS_CHORD_ERROR, false, STEP_TRANS_ANGLE_RES, true);
    int aNbNodes = 0;
    int aNbTriangles = 0;
    for (TopExp_Explorer anExpSF(theSolid, TopAbs_FACE); anExpSF.More(); anExpSF.Next()) {
        TopLoc_Location aLoc;
        Handle(Poly_Triangulation) aTriangulation = BRep_Tool::Triangulation(TopoDS::Face(anExpSF.Current()), aLoc);
        if (!aTriangulation.IsNull()) {
            aNbNodes += aTriangulation->NbNodes();
            aNbTriangles += aTriangulation->NbTriangles();
        }
    }

    stl_file stl;
    stl.stats.type = inmemory;
    stl.stats.number_of_facets = (uint32_t)aNbTriangles;
    stl.stats.original_num_facets = stl.stats.number_of_facets;
    stl_allocate(&stl);

    std::vector<Vec3f> points;
    points.reserve(aNbNodes);
    //BBS: count faces missing triangulation
    Standard_Integer aNbFacesNoTri = 0;
    //BBS: fill temporary triangulation
    Standard_Integer aNodeOffset = 0;
    Standard_Integer aTriangleOffet = 0;
    for (TopExp_Explorer anExpSF(theSolid, TopAbs_FACE); anExpSF.More(); anExpSF.Next()) {
        const TopoDS_Shape& aFace = anExpSF.Current();
        TopLoc_Location aLoc;
        Handle(Poly_Triangulation) aTriangulation = BRep_Tool::Triangulation(TopoDS::Face(aFace), aLoc);
        if (aTriangulation.IsNull()) {
            ++aNbFacesNoTri;
            continue;
        }
        //BBS: copy nodes
        gp_Trsf aTrsf = aLoc.Transformation();
        for (Standard_Integer aNodeIter = 1; aNodeIter <= aTriangulation->NbNodes(); ++aNodeIter) {
            gp_Pnt aPnt = aTriangulation->Node(aNodeIter);
            aPnt.Transform(aTrsf);
            points.emplace_back(std::move(Vec3f(aPnt.X(), aPnt.Y(), aPnt.Z())));
        }
        //BBS: copy triangles
        const TopAbs_Orientation anOrientation = anExpSF.Current().Orientation();
        for (Standard_Integer aTriIter = 1; aTriIter <= aTriangulation->NbTriangles(); ++aTriIter) {
            Poly_Triangle aTri = aTriangulation->Triangle(aTriIter);

            Standard_Integer anId[3];
            aTri.Get(anId[0], anId[1], anId[2]);
            if (anOrientation == TopAbs_REVERSED) {
                //BBS: swap 1, 2.
                Standard_Integer aTmpIdx = anId[1];
                anId[1] = anId[2];
                anId[2] = aTmpIdx;
            }
            //BBS: Update nodes according to the offset.
            anId[0] += aNodeOffset;
            anId[1] += aNodeOffset;
            anId[2] += aNodeOffset;
            //BBS: save triangles facets
            stl_facet facet;
            facet.vertex[0] = points[anId[0] - 1].cast<float>();
            facet.vertex[1] = points[anId[1] - 1].cast<float>();
            facet.vertex[2] = points[anId[2] - 1].cast<float>();
            facet.extra[0] = 0;
            facet.extra[1] = 0;
            stl_normal normal;
            stl_calculate_normal(normal, &facet);
            stl_normalize_vector(normal);
            facet.normal = normal;
            stl.facet_start[aTriangleOffet + aTriIter - 1] = facet;
        }

        aNodeOffset += aTriangulation->NbNodes();
        aTriangleOffet += aTriangulation->NbTriangles();
    }

    theMesh.from_stl(stl);
}

void load_text_shape(const char*text, const char* font, const float text_height, const float thickness, bool is_bold, bool is_italic, TextResult &text_result)
{
    if (thickness <= 0)
        return;

    Handle(Font_FontMgr) aFontMgr = Font_FontMgr::GetInstance();
    if (aFontMgr->GetAvailableFonts().IsEmpty())
        aFontMgr->InitFontDataBase();

    TopoDS_Shape aTextBase;
    Font_FontAspect aFontAspect = Font_FontAspect_UNDEFINED;
    if (is_bold && is_italic)
        aFontAspect = Font_FontAspect_BoldItalic;
    else if (is_bold)
        aFontAspect = Font_FontAspect_Bold;
    else if (is_italic)
        aFontAspect = Font_FontAspect_Italic;
    else
        aFontAspect = Font_FontAspect_Regular;

    if (!TextToBRep(text, font, text_height, aFontAspect, aTextBase, text_result.text_width))
        return;

    TopoDS_Shape aTextShape;
    if (!Prism(aTextBase, thickness, aTextShape))
        return;

    MakeMesh(aTextShape, text_result.text_mesh);
}

}; // namespace Slic3r
