#include "../../src/libslic3r/Gpu/GpuExactGeometry.hpp"
#include "../../src/libslic3r/Gpu/GpuBatchSafety.hpp"
#include "../../src/libslic3r/Gpu/VulkanSlicer.hpp"
#include <exception>
// Build this standalone fixture with CudaSlicer.cpp only: it includes the
// production implementation to test its private recorder without test hooks.
#include "../../src/libslic3r/Gpu/VulkanSlicer.cpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace {
#ifdef _WIN32
using Native = std::wstring;
Native env_value(const wchar_t* name) { const auto value = _wgetenv(name); return value ? value : L""; }
void env_set(const wchar_t* name, const Native& value) { if (_wputenv_s(name, value.c_str())) throw std::runtime_error("environment write failed"); }
#define NATIVE(value) L##value
#else
using Native = std::string;
Native env_value(const char* name) { const auto value = std::getenv(name); return value ? value : ""; }
void env_set(const char* name, const Native& value) {
    const int result = value.empty() ? unsetenv(name) : setenv(name, value.c_str(), 1);
    if (result) throw std::runtime_error("environment write failed");
}
#define NATIVE(value) value
#endif
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct RestoreEnvironment {
    Native sink = env_value(NATIVE("SLIC3R_GPU_DIAGNOSTICS"));
    Native backend = env_value(NATIVE("SLIC3R_GPU_BACKEND"));
    Native geometry = env_value(NATIVE("GPU_DIAGNOSTICS_TEST_GEOMETRY"));
    ~RestoreEnvironment() {
        env_set(NATIVE("SLIC3R_GPU_DIAGNOSTICS"), sink);
        env_set(NATIVE("SLIC3R_GPU_BACKEND"), backend);
        env_set(NATIVE("GPU_DIAGNOSTICS_TEST_GEOMETRY"), geometry);
    }
};
int launch_child(const fs::path& executable, const char* mode)
{
#ifdef _WIN32
    std::wstring command = L"\"" + executable.wstring() + L"\" " + std::wstring(mode, mode + std::strlen(mode));
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &start, &process) != 0,
            "CreateProcessW failed");
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return int(code);
#else
    const pid_t pid = fork();
    require(pid >= 0, "fork failed");
    if (pid == 0) {
        execl(executable.c_str(), executable.c_str(), mode, static_cast<char*>(nullptr));
        _exit(125);
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid, "waitpid failed");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 126;
#endif
}
std::string read_file(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::map<std::string,std::string> read_summary(const fs::path& path)
{
    require(fs::is_regular_file(path), "exit summary missing");
    std::ifstream stream(path);
    std::map<std::string,std::string> values;
    for (std::string line; std::getline(stream,line);) {
        const auto equal = line.find('=');
        require(equal != std::string::npos, "invalid summary record");
        require(values.emplace(line.substr(0,equal),line.substr(equal+1)).second, "duplicate summary key");
    }
    return values;
}
int child(const std::string& mode)
{
    using namespace Slic3r::Gpu;
    // Only ordinary production calls trigger registration. No GPU is compiled
    // into this fixture; checked geometry and CPU fallback remain real code.
    (void)VulkanSlicerBackend::backend_preference();
    VerticalIntersectionRequest request{{{0,2},{10,12}},3,8};
    VerticalIntersection reference;
    require(checked_vertical_reference(request,reference), "reference rejected");
    if (mode == "--fixture") {
        require(!VulkanSlicerBackend::dispatch_vertical_intersections({request}).dispatched, "unexpected GPU dispatch in CPU fixture");
        auto& registry = runtime_stats_registry();
        VerticalIntersectionBatch recorded;
        recorded.dispatched = true;
        recorded.queue_submissions = 1;
        registry.record_dispatch(3,recorded);
        registry.record_usage(3,3,false,"synthetic recorder fixture");
        VulkanTreeContourBatch failed_tree;
        failed_tree.diagnostic = "synthetic pre-submission tree failure";
        registry.record_tree_result(4,failed_tree);
        registry.record_tree_contour_broad_phase(4,7,"synthetic tree recorder fixture");
        VulkanTreeContourBatch accepted_tree;
        accepted_tree.dispatched = true;
        accepted_tree.may_intersect = {1,0,1,0};
        registry.record_tree_result(4,accepted_tree);
        registry.begin_slice();
        recorded.queue_submissions = 2;
        registry.record_dispatch(2,recorded);
        registry.record_usage(2,2,true,"synthetic fixture\nreason");
        // A callback registered later must run before the snapshot, while the
        // registry is still alive. ASan also checks the registry teardown order.
        std::atexit([] {
            runtime_stats_registry().record_usage(0,0,false,"later teardown\nreason");
        });
    }
    if (mode == "--vertical-failure")
        runtime_stats_registry().record_vertical_failure("synthetic CUDA failure");
    if (mode == "--tree-failure") {
        VulkanTreeContourBatch failed;
        failed.diagnostic = "synthetic Vulkan tree failure";
        runtime_stats_registry().record_tree_result(4,failed);
    }
    const fs::path sink(env_value(NATIVE("SLIC3R_GPU_DIAGNOSTICS")));
    if (!sink.empty() && (!fs::exists(sink) || fs::is_regular_file(sink)))
        require(!fs::exists(sink), "diagnostics performed per-batch file I/O");
    std::ofstream geometry(fs::path(env_value(NATIVE("GPU_DIAGNOSTICS_TEST_GEOMETRY"))),std::ios::binary);
    geometry << reference.numerator << '/' << reference.denominator << '\n';
    geometry.close();
    return 7;
}
}

int main(int argc, char** argv)
{
    try {
        if (argc == 2) return child(argv[1]);
        RestoreEnvironment restore;
#ifdef _WIN32
        std::wstring module(32768,L'\0');
        const DWORD length = GetModuleFileNameW(nullptr,module.data(),DWORD(module.size()));
        require(length > 0 && length < module.size(), "executable path unavailable");
        module.resize(length);
        const fs::path executable(module);
#else
        const fs::path executable = fs::absolute(argv[0]);
#endif
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const fs::path directory = fs::temp_directory_path() / fs::u8path("gpu-exit-진단-"+suffix);
        fs::create_directory(directory);
        auto run = [&](const std::string& name,const fs::path& sink,const Native& backend,const char* mode) {
            env_set(NATIVE("SLIC3R_GPU_DIAGNOSTICS"),sink.empty() ? Native{} : sink.native());
            env_set(NATIVE("SLIC3R_GPU_BACKEND"),backend);
            const auto geometry = directory / (name+".geometry");
            env_set(NATIVE("GPU_DIAGNOSTICS_TEST_GEOMETRY"),geometry.native());
            require(launch_child(executable,mode) == 7,"diagnostics changed child exit status");
            require(read_file(geometry) == "50/10\n","diagnostics changed checked geometry");
        };
        run("off",{},NATIVE("auto"),"--fixture");
        require(std::distance(fs::directory_iterator(directory),fs::directory_iterator{}) == 1,
                "no-env mode created extra files");
        const auto unicode = directory/fs::u8path("결과-診断.txt");
        run("unicode",unicode,NATIVE("auto"),"--fixture");
        const auto values = read_summary(unicode);
        require(values.at("environment_backend") == "auto","environment mode misreported");
        require(values.at("requested_backend") == "auto","auto mode lost");
        require(values.at("active_backend") == "Vulkan","recorded active backend lost");
        require(values.at("vertical_submitted") == "5","vertical submission totals reset by slice");
        require(values.at("vertical_accepted") == "5","accepted totals wrong");
        require(values.at("vertical_full_checks") == "5","full-check totals wrong");
        require(values.at("vertical_failures") == "1","failure totals wrong");
        require(values.at("queue_submissions") == "4","queue totals wrong");
        require(values.at("tree_submissions") == "1","tree submissions missing");
        require(values.at("tree_submitted_branches") == "4","tree branches missing");
        require(values.at("tree_contour_boxes") == "7","whole-contour box totals wrong");
        require(values.at("tree_validated_branches") == "4","validated tree branches missing");
        require(values.at("tree_negative_branches") == "2","validated tree negatives missing");
        require(values.at("tree_failures") == "1","tree failure count wrong");
        require(values.at("backend_reason") == "later teardown\\nreason","late teardown snapshot or escaped reason is wrong");
        for (const auto& mode : std::vector<Native>{NATIVE("cpu"),NATIVE("cuda"),NATIVE("vulkan"),NATIVE("auto"),NATIVE("unknown"),Native{}}) {
            const auto sink = directory/("mode-"+std::to_string(mode.size())+"-"+std::to_string(mode.empty()?0:int(mode.front()))+".txt");
            run("mode",sink,mode,"--getters-only");
            const auto summary = read_summary(sink);
            const bool known = mode == NATIVE("cuda") || mode == NATIVE("vulkan") || mode == NATIVE("auto") || mode == NATIVE("cpu");
            const std::string expected = known ? std::string(mode.begin(),mode.end()) : "cpu";
            require(summary.at("requested_backend") == expected,"effective mode misreported");
            require(summary.at("active_backend") == "CPU","getter-only run fabricated GPU activation");
            require(summary.at("queue_submissions") == "0","getter-only run fabricated GPU submission");
            require(!summary.at("backend_reason").empty(),"backend selection reason missing");
        }
        for (const auto& kind : std::vector<std::string>{"vertical","tree"}) {
            const auto sink = directory/(kind+"-failure.txt");
            const std::string mode = "--"+kind+"-failure";
            run(kind+"-failure",sink,NATIVE("auto"),mode.c_str());
            const auto summary = read_summary(sink);
            require(summary.at(kind+"_failures") == "1","failed backend count lost");
            require(summary.at("active_backend") == "CPU","failed backend reported as active");
            require(summary.at("backend_reason").find("failure") != std::string::npos,"backend failure reason lost");
        }
        run("directory-sink",directory,NATIVE("auto"),"--fixture");
        run("missing-parent",directory/"missing"/"sink.txt",NATIVE("auto"),"--fixture");
        require(!fs::exists(directory/"missing"),"diagnostics created missing directories");
#ifndef _WIN32
        if (fs::exists("/dev/full"))
            run("write-failure",fs::path("/dev/full"),NATIVE("auto"),"--fixture");
#endif
        std::cout << "exit diagnostics child-process fixtures passed; artifacts: " << directory.u8string() << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
