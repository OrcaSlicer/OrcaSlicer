#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define MP_EXPORT
#include "mediapipe/tasks/c/vision/image_segmenter/image_segmenter.h"
#include "mediapipe/tasks/c/vision/face_landmarker/face_landmarker.h"
#include <iostream>
#include <filesystem>
#include <string>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if(argc != 2) return 2;
        const std::filesystem::path root=argv[1];
        HMODULE dll=LoadLibraryExW((root / L"libmediapipe.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!dll) { std::cerr << "dll_missing_or_unloadable=" << GetLastError() << '\n'; return 3; }
#define FN(name) auto fn_##name=reinterpret_cast<decltype(&name)>(GetProcAddress(dll,#name)); if(!fn_##name) throw std::runtime_error("missing export " #name)
        FN(MpErrorFree); FN(MpImageSegmenterCreate); FN(MpImageSegmenterClose); FN(MpFaceLandmarkerCreate); FN(MpFaceLandmarkerClose);
        auto failure=[&](const char* name,MpStatus status,char* error,bool no_instance) {
            std::cout << name << " status=" << int(status) << " null_task=" << no_instance << " error=" << (error?error:"") << '\n';
            if(error) fn_MpErrorFree(error);
            if(status==0 || !no_instance) throw std::runtime_error("invalid resource unexpectedly accepted");
        };
        const auto missing=(root/"intentionally-missing-model.task").string();
        const char corrupt[]="not a tflite model";
        for(bool bad_buffer: {false,true}) {
            MpBaseOptions base{}; base.file_descriptor=-1; base.delegate=MP_DELEGATE_CPU; base.host_system=MP_HOST_SYSTEM_WINDOWS;
            if(bad_buffer) { base.model_asset_buffer=corrupt; base.model_asset_buffer_count=sizeof(corrupt); }
            else base.model_asset_path=missing.c_str();
            MpImageSegmenterOptions body{}; body.base_options=base; body.running_mode=MP_RUNNING_MODE_IMAGE; body.output_confidence_masks=true;
            MpImageSegmenterPtr segmenter=nullptr; char* error=nullptr;
            auto status=fn_MpImageSegmenterCreate(&body,&segmenter,&error);
            failure(bad_buffer?"corrupt_body":"missing_body",status,error,segmenter==nullptr);
            MpFaceLandmarkerOptions face{}; face.base_options=base; face.running_mode=MP_RUNNING_MODE_IMAGE; face.num_faces=4;
            MpFaceLandmarkerPtr landmarker=nullptr; error=nullptr;
            status=fn_MpFaceLandmarkerCreate(&face,&landmarker,&error);
            failure(bad_buffer?"corrupt_face":"missing_face",status,error,landmarker==nullptr);
        }
        FreeLibrary(dll);
        std::cout << "resource_failure_checks=4 passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
