#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticMaterialRegions.hpp"
#include <algorithm>
#include <limits>

using namespace Slic3r;
using namespace Slic3r::AI::SemanticColoring;

namespace {
const Color dark {.13f,.12f,.14f}, white {.96f,.97f,.98f}, gray {.55f,.54f,.53f}, red {.85f,.08f,.05f};
const std::vector<Color> palette {dark,white,gray,red};
struct Fixture {
    MeshSnapshot source;
    Analysis analysis;
    FaceColors paint;
    int width;
    explicit Fixture(int size=40,Color color={.16f,.16f,.16f},Label label=Label::Hair,Color target=dark) : width(size) {
        for (int y=0;y<=size;++y) for (int x=0;x<=size;++x) source.mesh.vertices.emplace_back(float(x),float(y),0.f);
        for (int y=0;y<size;++y) for (int x=0;x<size;++x) {
            const int a=y*(size+1)+x,b=a+1,c=a+size+1,d=c+1;
            source.mesh.indices.emplace_back(a,b,c);source.mesh.indices.emplace_back(b,d,c);
        }
        source.geometry_id="material-region-fixture";source.content_id="source-color-fixture";
        const size_t count=source.mesh.indices.size();
        source.face_colors.assign(count,{color[0],color[1],color[2],1.f});
        analysis.geometry_id=source.geometry_id;analysis.content_id=source.content_id;
        analysis.person_detected=true;analysis.face_labels.assign(count,label);analysis.face_confidence.assign(count,.95f);
        for (size_t face=0;face<count;++face) paint.emplace_back(face,target);
    }
    size_t cell(int x,int y) const { return size_t(y*width+x)*2; }
    void set(size_t face,Color color,Label label,float confidence,Color target) {
        source.face_colors[face]={color[0],color[1],color[2],1.f};
        analysis.face_labels[face]=label;analysis.face_confidence[face]=confidence;paint[face].second=target;
    }
    void island(int x,int y,Color color={.45f,.45f,.45f},Label label=Label::Hair,float confidence=.95f) {
        set(cell(x,y),color,label,confidence,gray);set(cell(x,y)+1,color,label,confidence,gray);
    }
    void split_vertices(float gap=0.f) {
        std::vector<Vec3f> split;
        for (size_t face=0;face<source.mesh.indices.size();++face) {
            const auto original=source.mesh.indices[face];
            for (int corner=0;corner<3;++corner) {
                Vec3f v=source.mesh.vertices[original[corner]];v.z()+=gap*float(face%2);
                source.mesh.indices[face][corner]=int(split.size());split.push_back(v);
            }
        }
        source.mesh.vertices=std::move(split);
    }
    void refine() { refine_material_patches(source,analysis,palette,{},paint); }
};
}

TEST_CASE("Supported skin gaps inherit a neighboring skin material without changing lips or hair", "[SemanticMaterialRegions][Regression]")
{
    // These three samples follow a real nose-shadow ramp. The path is continuous
    // in chromaticity but spans enough lightness that the regular palette metric
    // must not be reused for intrinsic skin-material recovery.
    const Color skin {.7373f,.4824f,.4065f}, transition {.6105f,.3542f,.2889f},
        shadow {.5098f,.2510f,.1908f};
    Fixture fixture(12,skin,Label::FaceSkin,skin);
    const size_t recognized=fixture.cell(5,5),uncertain=fixture.cell(5,5)+1;
    fixture.set(recognized,shadow,Label::FaceSkin,.85f,red);
    fixture.set(uncertain,shadow,Label::Unknown,0.f,red);
    std::vector<size_t> gaps {recognized,uncertain};
    for (int y=4;y<=6;++y) for (int x=4;x<=6;++x) if (x!=5 || y!=5) {
        const size_t first=fixture.cell(x,y);
        fixture.set(first,transition,Label::FaceSkin,.85f,red);
        fixture.set(first+1,transition,Label::Unknown,0.f,red);
        gaps.push_back(first);gaps.push_back(first+1);
    }
    fixture.set(fixture.cell(8,8),red,Label::Lips,.95f,red);
    fixture.set(fixture.cell(2,2),dark,Label::Hair,.95f,dark);
    fixture.paint.erase(std::remove_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
        return std::find(gaps.begin(),gaps.end(),entry.first)!=gaps.end();
    }),fixture.paint.end());
    refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
    const auto assigned=[&](size_t face) {
        const auto found=std::find_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {return entry.first==face;});
        return found==fixture.paint.end()?Color {}:found->second;
    };
    CHECK(assigned(recognized)==skin);
    CHECK(assigned(uncertain)==skin);
    CHECK(assigned(fixture.cell(8,8))==red);
    CHECK(assigned(fixture.cell(2,2))==dark);
}

TEST_CASE("A disconnected skin colored patch has no material donor", "[SemanticMaterialRegions][Regression]")
{
    const Color skin {.80f,.59f,.48f};
    Fixture fixture(8,skin,Label::FaceSkin,skin);
    const size_t uncertain=fixture.cell(4,4);
    fixture.set(uncertain,skin,Label::Unknown,0.f,red);
    fixture.paint.erase(std::remove_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
        return entry.first==uncertain;
    }),fixture.paint.end());
    fixture.split_vertices(.00001f);
    const auto previous=fixture.paint;
    refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
    CHECK(fixture.paint==previous);
}

TEST_CASE("A skin colored gap beside a reliable lip is not painted over", "[SemanticMaterialRegions][Regression]")
{
    const Color skin {.80f,.59f,.48f}, shadow {.78f,.57f,.46f};
    Fixture fixture(12,skin,Label::FaceSkin,skin);
    const size_t lip=fixture.cell(5,5),uncertain=lip+1;
    fixture.set(lip,red,Label::Lips,.95f,red);
    fixture.set(uncertain,shadow,Label::Unknown,0.f,red);
    fixture.paint.erase(std::remove_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
        return entry.first==uncertain;
    }),fixture.paint.end());
    const auto previous=fixture.paint;
    refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
    CHECK(fixture.paint==previous);
}

TEST_CASE("An isolated unknown dark assignment enclosed by skin is filled but a hair boundary blocks it",
          "[SemanticMaterialRegions][Regression]")
{
    const Color skin {.7373f,.4824f,.4065f}, shadow {.70f,.45f,.37f};
    for (bool hair_boundary : {false,true}) {
        DYNAMIC_SECTION("hair boundary " << hair_boundary) {
            Fixture fixture(12,skin,Label::FaceSkin,skin);
            const size_t hole=fixture.cell(5,5);
            fixture.set(hole,shadow,Label::Unknown,0.f,dark);
            if (hair_boundary) fixture.set(hole+1,shadow,Label::Hair,.95f,dark);
            refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
            CHECK(fixture.paint[hole].second==(hair_boundary?dark:skin));
            if (hair_boundary) CHECK(fixture.paint[hole+1].second==dark);
        }
    }
}

TEST_CASE("A reliable dark face-skin shadow enclosed by skin ignores adjacent hair but preserves face details",
          "[SemanticMaterialRegions][Regression][FaceSkin]")
{
    const Color skin {.7373f,.4824f,.4065f}, shadow {.18f,.16f,.15f};
    for (Label boundary : {Label::Hair, Label::Eyebrow}) {
        DYNAMIC_SECTION("boundary " << int(boundary)) {
            Fixture fixture(12,skin,Label::FaceSkin,skin);
            const size_t hole=fixture.cell(5,5),detail=hole+1;
            fixture.set(hole,shadow,Label::FaceSkin,.88f,dark);
            fixture.set(detail,shadow,boundary,.95f,dark);
            fixture.paint.erase(std::remove_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                return entry.first==hole;
            }),fixture.paint.end());
            refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
            const auto assigned=std::find_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                return entry.first==hole;
            });
            if (boundary==Label::Hair) {
                REQUIRE(assigned!=fixture.paint.end());
                CHECK(assigned->second==skin);
            } else {
                CHECK(assigned==fixture.paint.end());
            }
            const auto protected_detail=std::find_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                return entry.first==detail;
            });
            REQUIRE(protected_detail!=fixture.paint.end());
            CHECK(protected_detail->second==dark);
        }
    }
}

TEST_CASE("A dark face-skin ear gap uses the shared reliable-confidence threshold",
          "[SemanticMaterialRegions][Regression][FaceSkin]")
{
    const Color skin {.7373f,.4824f,.4065f}, shadow {.18f,.16f,.15f};
    for (const auto& item : {std::pair<float, bool>{minimum_confidence, true},
                             std::pair<float, bool>{minimum_confidence - .01f, false}}) {
        DYNAMIC_SECTION("confidence " << item.first) {
            Fixture fixture(12,skin,Label::FaceSkin,skin);
            const size_t hole=fixture.cell(5,5);
            fixture.set(hole,shadow,Label::FaceSkin,item.first,dark);
            fixture.paint.erase(std::remove_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                return entry.first==hole;
            }),fixture.paint.end());
            refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
            const auto assigned=std::find_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                return entry.first==hole;
            });
            CHECK((assigned!=fixture.paint.end())==item.second);
            if (item.second) CHECK(assigned->second==skin);
        }
    }
}

TEST_CASE("A disconnected dark ear fold needs nearby frozen skin and hair support",
          "[SemanticMaterialRegions][Regression][FaceSkin]")
{
    const Color skin {.7373f,.4824f,.4065f}, shadow {.18f,.16f,.15f};
    for (bool detail : {false,true}) {
        DYNAMIC_SECTION("detail barrier " << detail) {
            Fixture fixture(8,skin,Label::FaceSkin,skin);
            const size_t fold=fixture.cell(4,4);
            fixture.set(fold,shadow,Label::FaceSkin,.88f,dark);
            fixture.set(fold+1,shadow,Label::Hair,.95f,dark);
            fixture.paint.erase(std::remove_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                return entry.first==fold;
            }),fixture.paint.end());
            const size_t donor=fixture.cell(0,0),detail_face=fixture.cell(3,4)+1;
            if (detail) fixture.set(detail_face,dark,Label::Iris,.95f,dark);
            fixture.split_vertices(.00001f);
            const auto place_near=[&](size_t face,float offset) {
                for (int corner=0;corner<3;++corner)
                    fixture.source.mesh.vertices[fixture.source.mesh.indices[face][corner]] =
                        fixture.source.mesh.vertices[fixture.source.mesh.indices[fold][corner]] + Vec3f(0.f,0.f,offset);
            };
            place_near(donor,.00001f);
            place_near(fold+1,-.00001f);
            if (detail) place_near(detail_face,.00002f);
            refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
            const auto assigned=std::find_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                return entry.first==fold;
            });
            if (detail) CHECK(assigned==fixture.paint.end());
            else {
                REQUIRE(assigned!=fixture.paint.end());
                CHECK(assigned->second==skin);
            }
        }
    }
}

TEST_CASE("A weak ear-rim skin vote needs independent compatible skin and hair support",
          "[SemanticMaterialRegions][Regression][FaceSkin]")
{
    const Color skin {.7373f,.4824f,.4065f}, shadow {.26f,.22f,.20f};
    for (const bool nearby_hair : {false, true}) for (const bool compatible_skin : {false, true})
        for (const bool detail : {false, true}) {
            DYNAMIC_SECTION("hair " << nearby_hair << " compatible skin " << compatible_skin <<
                            " detail " << detail) {
                Fixture fixture(8,skin,Label::FaceSkin,skin);
                const size_t fold=fixture.cell(4,4), donor=fixture.cell(0,0),
                             hair=fold+1, detail_face=fixture.cell(3,4)+1;
                fixture.set(fold,shadow,Label::FaceSkin,.40f,dark);
                fixture.set(donor,compatible_skin?shadow:Color{.55f,.18f,.12f},Label::FaceSkin,.95f,skin);
                fixture.set(hair,shadow,nearby_hair?Label::Hair:Label::Background,.95f,dark);
                if (detail) fixture.set(detail_face,dark,Label::Iris,.95f,dark);
                fixture.paint.erase(std::remove_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                    return entry.first==fold;
                }),fixture.paint.end());
                fixture.split_vertices(.00001f);
                const auto place_near=[&](size_t face,float offset) {
                    for (int corner=0;corner<3;++corner)
                        fixture.source.mesh.vertices[fixture.source.mesh.indices[face][corner]] =
                            fixture.source.mesh.vertices[fixture.source.mesh.indices[fold][corner]] + Vec3f(0.f,0.f,offset);
                };
                place_near(donor,.00001f);
                place_near(hair,-.00001f);
                if (detail) place_near(detail_face,.00002f);
                refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
                const auto assigned=std::find_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                    return entry.first==fold;
                });
                const bool expected=nearby_hair && compatible_skin && !detail;
                CHECK((assigned!=fixture.paint.end())==expected);
                if (expected) CHECK(assigned->second==skin);
            }
        }
}

TEST_CASE("A weak ear rim may borrow a skin slot across an opposed thin shell only with local skin support",
          "[SemanticMaterialRegions][Regression][FaceSkin]")
{
    const Color skin {.7373f,.4824f,.4065f}, shadow {.26f,.22f,.20f};
    for (const bool local_support : {false, true}) {
        DYNAMIC_SECTION("local support " << local_support) {
            Fixture fixture(8,skin,Label::FaceSkin,skin);
            const size_t fold=fixture.cell(4,4), support=fixture.cell(3,4)+1,
                         donor=fixture.cell(0,0), hair=fold+1;
            fixture.set(fold,shadow,Label::FaceSkin,.40f,dark);
            fixture.set(support,shadow,local_support?Label::FaceSkin:Label::Background,.95f,dark);
            fixture.set(donor,shadow,Label::FaceSkin,.95f,skin);
            fixture.set(hair,shadow,Label::Hair,.95f,dark);
            fixture.paint.erase(std::remove_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                return entry.first==fold;
            }),fixture.paint.end());
            fixture.split_vertices(.00001f);
            const auto place_near=[&](size_t face,float offset) {
                for (int corner=0;corner<3;++corner)
                    fixture.source.mesh.vertices[fixture.source.mesh.indices[face][corner]] =
                        fixture.source.mesh.vertices[fixture.source.mesh.indices[fold][corner]] + Vec3f(0.f,0.f,offset);
            };
            place_near(support,.00001f);
            place_near(donor,.00002f);
            place_near(hair,-.00001f);
            std::swap(fixture.source.mesh.indices[donor][0],fixture.source.mesh.indices[donor][1]);
            refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
            const auto assigned=std::find_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
                return entry.first==fold;
            });
            CHECK((assigned!=fixture.paint.end())==local_support);
            if (local_support) CHECK(assigned->second==skin);
        }
    }
}

TEST_CASE("A face-detail collar blocks a chain of dark face-skin predictions",
          "[SemanticMaterialRegions][Regression][FaceSkin]")
{
    const Color skin {.7373f,.4824f,.4065f}, shadow {.18f,.16f,.15f};
    Fixture fixture(12,skin,Label::FaceSkin,skin);
    const size_t detail=fixture.cell(4,5),first=detail+1,second=fixture.cell(5,5),third=second+1;
    fixture.set(detail,shadow,Label::Iris,.95f,dark);
    for (size_t id : {first,second,third}) {
        fixture.set(id,shadow,Label::FaceSkin,.88f,dark);
        fixture.paint.erase(std::remove_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
            return entry.first==id;
        }),fixture.paint.end());
    }
    refine_material_patches(fixture.source,fixture.analysis,{dark,white,gray,red,skin},{},fixture.paint);
    for (size_t id : {first,second,third})
        CHECK(std::none_of(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) { return entry.first==id; }));
    CHECK(std::find_if(fixture.paint.begin(),fixture.paint.end(),[&](const auto& entry) {
        return entry.first==detail && entry.second==dark;
    })!=fixture.paint.end());
}

TEST_CASE("A small neutral hair highlight inherits the surrounding assigned material across exact vertex seams", "[SemanticMaterialRegions][Regression]")
{
    Fixture fixture;fixture.island(20,20);fixture.split_vertices();
    for (size_t face=0;face<fixture.source.mesh.indices.size();++face)
        for (int corner=0;corner<3;++corner) fixture.source.vertex_colors.push_back(fixture.source.face_colors[face]);
    fixture.source.face_colors.clear();
    const auto geometry=fixture.source.mesh.indices;const auto colors=fixture.source.face_colors;
    const auto vertex_colors=fixture.source.vertex_colors;
    const auto labels=fixture.analysis.face_labels;const auto confidence=fixture.analysis.face_confidence;
    fixture.refine();
    CHECK(fixture.paint[fixture.cell(20,20)].second==dark);
    CHECK(fixture.paint[fixture.cell(20,20)+1].second==dark);
    CHECK(fixture.source.mesh.indices==geometry);
    CHECK(fixture.source.face_colors==colors);
    CHECK(fixture.source.vertex_colors==vertex_colors);
    CHECK(fixture.analysis.face_labels==labels);
    CHECK(fixture.analysis.face_confidence==confidence);
}

TEST_CASE("Disconnected or opposed surfaces cannot donate a hair material through proximity", "[SemanticMaterialRegions]")
{
    Fixture gap;gap.island(20,20);gap.split_vertices(.00001f);
    const auto before=gap.paint;gap.refine();CHECK(gap.paint==before);
    Fixture reversed;reversed.island(20,20);
    for (size_t id : {reversed.cell(20,20),reversed.cell(20,20)+1}) std::swap(reversed.source.mesh.indices[id][0],reversed.source.mesh.indices[id][1]);
    const auto original=reversed.paint;reversed.refine();CHECK(reversed.paint==original);
}

TEST_CASE("Large gray or white hair regions and chromatic dyed streaks retain their materials", "[SemanticMaterialRegions]")
{
    Fixture fixture;
    for (int y=10;y<20;++y) for (int x=10;x<20;++x) fixture.island(x,y);
    // Supported large gray region is preserved; real red is outside the neutral patch rule.
    fixture.island(30,30,red);
    for (size_t id : {fixture.cell(30,30),fixture.cell(30,30)+1}) fixture.paint[id].second=red;
    fixture.refine();
    CHECK(fixture.paint[fixture.cell(15,15)].second==gray);
    CHECK(fixture.paint[fixture.cell(30,30)].second==red);
    Fixture white_hair(40,{.96f,.96f,.96f},Label::Hair,white);const auto before=white_hair.paint;
    white_hair.refine();CHECK(white_hair.paint==before);
}

TEST_CASE("Duplicate and nonmanifold triangles cannot form a material bridge", "[SemanticMaterialRegions]")
{
    Fixture fixture;fixture.island(20,20);
    for (size_t id : {fixture.cell(20,20),fixture.cell(20,20)+1}) {
        const size_t duplicate=fixture.source.mesh.indices.size();
        fixture.source.mesh.indices.push_back(fixture.source.mesh.indices[id]);
        fixture.source.face_colors.push_back(fixture.source.face_colors[id]);
        fixture.analysis.face_labels.push_back(Label::Hair);fixture.analysis.face_confidence.push_back(.95f);
        fixture.paint.emplace_back(duplicate,gray);
    }
    const auto before=fixture.paint;fixture.refine();CHECK(fixture.paint==before);
}

TEST_CASE("A local hair correction inherits the actual assigned filament even when palette slots move", "[SemanticMaterialRegions]")
{
    Fixture fixture;fixture.island(20,20);
    const Color alternative {.10f,.14f,.12f};
    for (auto& paint : fixture.paint) if (paint.second==dark) paint.second=alternative;
    refine_material_patches(fixture.source,fixture.analysis,{white,gray,alternative,red},{},fixture.paint);
    CHECK(fixture.paint[fixture.cell(20,20)].second==alternative);
}

TEST_CASE("All face skin eye lip and accessory labels remain protected even at low confidence", "[SemanticMaterialRegions]")
{
    for (Label label : {Label::FaceSkin,Label::BodySkin,Label::Accessories,Label::Lips,Label::MouthInterior,Label::EyeSclera,Label::Iris,Label::Eyebrow}) {
        DYNAMIC_SECTION("protected label "<<int(label)) {
            Fixture fixture;fixture.island(20,20,{.45f,.45f,.45f},label,.1f);
            const auto before=fixture.paint;fixture.refine();CHECK(fixture.paint==before);
        }
    }
}

TEST_CASE("A gray clothing island inside dark clothes does not borrow an unrelated hair target", "[SemanticMaterialRegions]")
{
    Fixture fixture(40,{.16f,.16f,.16f},Label::Clothes,dark);fixture.island(20,20,{.45f,.45f,.45f},Label::Clothes);
    fixture.set(fixture.cell(1,1),{.16f,.16f,.16f},Label::Hair,.95f,dark);
    const auto before=fixture.paint;fixture.refine();CHECK(fixture.paint==before);
}

TEST_CASE("Soft shadows in a supported white garment use its existing white material", "[SemanticMaterialRegions][Regression]")
{
    Fixture fixture(40,{.96f,.96f,.96f},Label::Clothes,white);
    for (int y=0;y<40;++y) for (int x=0;x<12;++x) {
        const float shade=.48f+.04f*x;
        for (size_t id : {fixture.cell(x,y),fixture.cell(x,y)+1}) fixture.set(id,{shade,shade,shade},Label::Unknown,.2f,gray);
    }
    fixture.refine();
    CHECK(fixture.paint[fixture.cell(0,20)].second==white);
    CHECK(fixture.paint[fixture.cell(7,20)].second==white);
    CHECK(fixture.paint[fixture.cell(20,20)].second==white);
}

TEST_CASE("A supported gray stripe on white fabric and a wholly gray garment retain their materials", "[SemanticMaterialRegions][Regression]")
{
    Fixture fixture(40,{.96f,.96f,.96f},Label::Clothes,white);
    for (int y=0;y<40;++y) for (int x=17;x<21;++x)
        for (size_t id : {fixture.cell(x,y),fixture.cell(x,y)+1}) fixture.set(id,{.48f,.48f,.48f},Label::Clothes,.95f,gray);
    fixture.refine();CHECK(fixture.paint[fixture.cell(18,20)].second==gray);
    Fixture gray_cloth(40,{.48f,.48f,.48f},Label::Clothes,gray);const auto before=gray_cloth.paint;
    gray_cloth.refine();CHECK(gray_cloth.paint==before);
}

TEST_CASE("Hair exposed beside white clothing retains its own reliable material", "[SemanticMaterialRegions]")
{
    Fixture fixture(40,{.96f,.96f,.96f},Label::Clothes,white);
    fixture.island(20,20,{.55f,.55f,.55f},Label::Hair,.95f);
    fixture.island(24,24,{.55f,.55f,.55f},Label::BodySkin,.1f);
    fixture.refine();CHECK(fixture.paint[fixture.cell(20,20)].second==gray);
    CHECK(fixture.paint[fixture.cell(24,24)].second==gray);
}

TEST_CASE("A small sharply bounded material survives when the surrounding white garment grows", "[SemanticMaterialRegions][Regression]")
{
    for (int size : {40,80}) {
        DYNAMIC_SECTION("white garment width "<<size) {
            Fixture fixture(size,{.96f,.96f,.96f},Label::Clothes,white);
            fixture.island(20,20,{.48f,.48f,.48f},Label::Clothes);
            fixture.refine();
            CHECK(fixture.paint[fixture.cell(20,20)].second==gray);
            CHECK(fixture.paint[fixture.cell(20,20)+1].second==gray);
        }
    }
}

TEST_CASE("Muted green cloth and brown hair with reliable surface support retain their materials", "[SemanticMaterialRegions][Regression]")
{
    Fixture fixture(120,{.96f,.96f,.96f},Label::Clothes,white);
    // Continuous brightness transitions intentionally remove the sharp-edge
    // evidence. The green/brown source chromaticity must still be respected.
    for (int y=0;y<40;++y) for (int x=0;x<12;++x) {
        const float shade=.29f+.055f*x;
        const Color color=y<20?Color{shade,shade+.038f,shade+.025f}:Color{shade+.07f,shade,shade-.03f};
        const Label label=y<20?Label::Clothes:(y<30?Label::Hair:Label::Unknown);
        for (size_t id : {fixture.cell(x,y),fixture.cell(x,y)+1}) fixture.set(id,color,label,y<20?.95f:.2f,dark);
    }
    // Warm/brown source color alone is ambiguous. The uncertain hair faces
    // have nearby reliable hair on the same surface, unlike a warm cloth shadow.
    for (int y : {25,35}) fixture.analysis.face_confidence[fixture.cell(2,y)+1]=.95f;
    fixture.analysis.face_labels[fixture.cell(2,35)+1]=Label::Hair;
    fixture.refine();
    CHECK(fixture.paint[fixture.cell(0,10)].second==dark);
    CHECK(fixture.paint[fixture.cell(2,25)].second==dark);
    CHECK(fixture.paint[fixture.cell(2,35)].second==dark);
    CHECK(fixture.paint[fixture.cell(20,20)].second==white);
}

TEST_CASE("Warm white garment shadows without local hair evidence remain white", "[SemanticMaterialRegions][Regression]")
{
    Fixture fixture(120,{.96f,.96f,.96f},Label::Clothes,white);
    fixture.island(20,20,{.55f,.48f,.44f},Label::Unknown,0.f);
    fixture.island(24,24,{.55f,.48f,.44f},Label::Hair,.3f);
    fixture.refine();
    CHECK(fixture.paint[fixture.cell(20,20)].second==white);
    CHECK(fixture.paint[fixture.cell(24,24)].second==white);
}

TEST_CASE("Neutral white garment gaps survive weak hair labels and unsupported sharp shadows", "[SemanticMaterialRegions][Regression]")
{
    Fixture fixture(40,{.96f,.96f,.96f},Label::Clothes,white);
    fixture.island(20,20,{.55f,.55f,.55f},Label::Hair,.3f);
    fixture.island(24,24,{.55f,.55f,.55f},Label::Unknown,0.f);
    fixture.refine();
    CHECK(fixture.paint[fixture.cell(20,20)].second==white);
    CHECK(fixture.paint[fixture.cell(24,24)].second==white);
}

TEST_CASE("One isolated clothing label cannot turn an uncertain white garment shadow into a separate material", "[SemanticMaterialRegions][Regression]")
{
    Fixture fixture(40,{.96f,.96f,.96f},Label::Clothes,white);
    for (int y=18;y<22;++y) for (int x=18;x<22;++x)
        fixture.island(x,y,{.55f,.55f,.55f},Label::Unknown,0.f);
    fixture.set(fixture.cell(20,20),{.55f,.55f,.55f},Label::Clothes,.95f,gray);
    fixture.refine();
    CHECK(fixture.paint[fixture.cell(20,20)].second==white);
    CHECK(fixture.paint[fixture.cell(19,19)].second==white);
}

TEST_CASE("A neutral gray stripe retains its material when its own brightness varies", "[SemanticMaterialRegions][Regression]")
{
    Fixture fixture(120,{.96f,.96f,.96f},Label::Clothes,white);
    for (int y=0;y<120;++y) for (int x=58;x<62;++x) {
        const float shade=.35f+.20f*float(y)/119.f;
        for (size_t id : {fixture.cell(x,y),fixture.cell(x,y)+1}) fixture.set(id,{shade,shade,shade},Label::Clothes,.95f,gray);
    }
    fixture.refine();
    CHECK(fixture.paint[fixture.cell(60,20)].second==gray);
    CHECK(fixture.paint[fixture.cell(60,90)].second==gray);
}

TEST_CASE("A locally pale edge cannot erase a dyed inner garment with a brightness gradient", "[SemanticMaterialRegions][Regression]")
{
    for (int size : {120,160}) {
        DYNAMIC_SECTION("surrounding white garment width "<<size) {
            Fixture fixture(size,{.96f,.96f,.96f},Label::Clothes,white);
            for (int y=18;y<25;++y) for (int x=18;x<25;++x)
                for (size_t id : {fixture.cell(x,y),fixture.cell(x,y)+1})
                    fixture.set(id,{.84f,.89f,.85f},Label::Clothes,.95f,white);
            for (int y=20;y<22;++y) for (int x=20;x<22;++x) {
                const Color source=(x==20 && y==20)?Color{.50f,.54f,.51f}:Color{.26f,.32f,.28f};
                for (size_t id : {fixture.cell(x,y),fixture.cell(x,y)+1})
                    fixture.set(id,source,Label::Clothes,.95f,dark);
            }
            fixture.refine();
            CHECK(fixture.paint[fixture.cell(20,20)].second==dark);
            CHECK(fixture.paint[fixture.cell(20,20)+1].second==dark);
            CHECK(fixture.paint[fixture.cell(21,21)].second==dark);
            CHECK(fixture.paint[fixture.cell(19,19)].second==white);
        }
    }
}

TEST_CASE("Warm white shadows near a hair junction retain uncertain material boundaries", "[SemanticMaterialRegions][Regression]")
{
    for (bool nearby_hair : {false,true}) {
        DYNAMIC_SECTION("nearby hair "<<nearby_hair) {
            Fixture fixture(120,{.96f,.96f,.96f},Label::Clothes,white);
            for (int y=18;y<23;++y) for (int x=18;x<24;++x)
                for (size_t id : {fixture.cell(x,y),fixture.cell(x,y)+1})
                    fixture.set(id,{.84f,.89f,.85f},Label::Clothes,.95f,white);
            for (int x=20;x<22;++x) {
                const Color source=x==20?Color{.50f,.54f,.51f}:Color{.30f,.33f,.31f};
                for (size_t id : {fixture.cell(x,20),fixture.cell(x,20)+1})
                    fixture.set(id,source,Label::Clothes,.95f,gray);
            }
            // The bright clothing between this hair and the shadow remains
            // a barrier to borrowing hair color, but cannot hide junction risk.
            if (nearby_hair)
                for (size_t id : {fixture.cell(24,20),fixture.cell(24,20)+1})
                    fixture.set(id,{.20f,.13f,.10f},Label::Hair,.95f,dark);
            fixture.refine();
            CHECK(fixture.paint[fixture.cell(20,20)].second==(nearby_hair?gray:white));
            CHECK(fixture.paint[fixture.cell(21,20)+1].second==(nearby_hair?gray:white));
            CHECK(fixture.paint[fixture.cell(23,20)].second==white);
        }
    }
}

TEST_CASE("An uncertain neck contour beside protected skin cannot be absorbed into a white garment", "[SemanticMaterialRegions][Regression]")
{
    Fixture fixture(120,{.96f,.96f,.96f},Label::Clothes,white);
    fixture.island(20,20,{.55f,.55f,.55f},Label::Unknown,0.f);
    fixture.island(21,20,{.55f,.55f,.55f},Label::BodySkin,.1f);
    fixture.refine();
    CHECK(fixture.paint[fixture.cell(20,20)+1].second==gray);
    CHECK(fixture.paint[fixture.cell(21,20)].second==gray);
}

TEST_CASE("Missing assignments and invalid input cannot invent a material or partially update suggestions", "[SemanticMaterialRegions]")
{
    Fixture fixture;fixture.island(20,20);fixture.paint.clear();fixture.refine();CHECK(fixture.paint.empty());
    Fixture invalid;invalid.island(20,20);invalid.source.mesh.vertices.back().x()=std::numeric_limits<float>::quiet_NaN();
    const auto before=invalid.paint;invalid.refine();CHECK(invalid.paint==before);
    Fixture no_person;no_person.island(20,20);no_person.analysis.person_detected=false;
    const auto original=no_person.paint;no_person.refine();CHECK(no_person.paint==original);
}
