#ifndef slic3r_Format_STEP_hpp_
#define slic3r_Format_STEP_hpp_
#include "XCAFDoc_DocumentTool.hxx"
#include "XCAFApp_Application.hxx"
#include "XCAFDoc_ShapeTool.hxx"
#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>
#include <Message_ProgressIndicator.hxx>
#include <atomic>

namespace fs = boost::filesystem;

namespace Slic3r {

class TriangleMesh;
class ModelObject;

// load step stage
const int LOAD_STEP_STAGE_READ_FILE          = 0;
const int LOAD_STEP_STAGE_GET_SOLID          = 1;
const int LOAD_STEP_STAGE_GET_MESH           = 2;
const int LOAD_STEP_STAGE_NUM                = 3;
const int LOAD_STEP_STAGE_UNIT_NUM           = 5;

typedef std::function<void(int load_stage, int current, int total, bool& cancel)> ImportStepProgressFn;
typedef std::function<void(bool isUtf8)> StepIsUtf8Fn;

struct NamedSolid
{
    NamedSolid(const TopoDS_Shape& s,
               const std::string& n) : solid{ s }, name{ n } {
    }
    const TopoDS_Shape solid;
    const std::string  name;
    int tri_face_cout = 0;
};

//BBS: Load an step file into a provided model.
extern bool load_step(const char *path, Model *model,
                      bool& is_cancel,
                      double linear_defletion = 0.003,
                      double angle_defletion = 0.5,
                      bool isSplitCompound = false,
                      ImportStepProgressFn proFn = nullptr,
                      StepIsUtf8Fn isUtf8Fn = nullptr,
                      long& mesh_face_num = *(new long(-1)));

//BBS: Used to detect what kind of encoded type is used in name field of step
// If is encoded in UTF8, the file don't need to be handled, then return the original path directly.
// If is encoded in GBK, then translate to UTF8 and generate a new temporary step file.
// If is encoded in Other type, we can't handled, then treat as UTF8. In this case, the name is garbage
// characters.
// By preprocessing, at least we can avoid garbage characters if the name field is encoded by GBK.
class StepPreProcessor {
    enum class EncodedType : unsigned char
    {
        UTF8,
        GBK,
        OTHER
    };

public:
    bool preprocess(const char* path, std::string &output_path);
    static bool isUtf8File(const char* path);
    static bool isUtf8(const std::string str);
private:
    static bool isGBK(const std::string str);
    static int preNum(const unsigned char byte);
    //BBS: default is UTF8 for most step file.
    EncodedType m_encode_type = EncodedType::UTF8;
};

class StepProgressIncdicator : public Message_ProgressIndicator
{
public:
    StepProgressIncdicator(std::atomic<bool>& stop_flag) : should_stop(stop_flag){}

    Standard_Boolean UserBreak() override { return should_stop.load(); }

    void Show(const Message_ProgressScope&, const Standard_Boolean) override {
        std::cout << "Progress: " << GetPosition() << "%" << std::endl;
    }
private:
    std::atomic<bool>& should_stop;
};

class Step
{
public:
    enum class Step_Status {
        LOAD_SUCCESS,
        LOAD_ERROR,
        CANCEL,
        MESH_SUCCESS,
        MESH_ERROR
    };
    Step(fs::path path, ImportStepProgressFn stepFn = nullptr, StepIsUtf8Fn isUtf8Fn = nullptr);
    Step(std::string path, ImportStepProgressFn stepFn = nullptr, StepIsUtf8Fn isUtf8Fn = nullptr);
    ~Step();
    Step_Status load();
    unsigned int get_triangle_num(double linear_defletion, double angle_defletion);
    unsigned int get_triangle_num_tbb(double linear_defletion, double angle_defletion);
    void clean_mesh_data();
    Step_Status mesh(Model* model,
                     bool& is_cancel,
                     bool isSplitCompound,
                     double linear_defletion = 0.003,
                     double angle_defletion = 0.5);

    std::atomic<bool> m_stop_mesh;
    void update_process(int load_stage, int current, int total, bool& cancel);
private:
    std::string m_path;
    ImportStepProgressFn m_stepFn;
    StepIsUtf8Fn m_utf8Fn;
    Handle(XCAFApp_Application) m_app = XCAFApp_Application::GetApplication();
    Handle(TDocStd_Document) m_doc;
    Handle(XCAFDoc_ShapeTool) m_shape_tool;
    std::vector<NamedSolid> m_name_solids;
};

}; // namespace Slic3r

#endif /* slic3r_Format_STEP_hpp_ */
