#pragma once
#include <string>
#include <functional>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <cstring>
#include <memory>
#include <boost/log/trivial.hpp>

#ifdef _WIN32
#include <windows.h>
#define FT_CALL __cdecl
using ModuleHandle = HMODULE;
#else
#include <dlfcn.h>
#define FT_CALL
using ModuleHandle = void *;
#endif

namespace Slic3r {

extern "C" {

struct ft_job_result
{
    int         ec;
    int         resp_ec;
    const char *json;
    const void *bin;
    uint32_t    bin_size;
};

struct ft_job_msg
{
    int         kind;
    const char *json;
};

struct FT_TunnelHandle;
struct FT_JobHandle;

typedef enum { FT_OK = 0, FT_EINVAL = -1, FT_ESTATE = -2, FT_EIO = -3, FT_ETIMEOUT = -4, FT_ECANCELLED = -5, FT_EXCEPTION = -6, FT_EUNKNOWN = -128 } ft_err;

}


inline void *sym_lookup_raw(ModuleHandle mh, const char *name)
{
#ifdef _WIN32
    return mh ? reinterpret_cast<void *>(::GetProcAddress(mh, name)) : nullptr;
#else
    return mh ? ::dlsym(mh, name) : nullptr;
#endif
}

template<class T> inline T sym_lookup(ModuleHandle mh, const char *name)
{
    void *p = sym_lookup_raw(mh, name);
    if (p) {
        return reinterpret_cast<T>(p);
    }
    else {
        BOOST_LOG_TRIVIAL(info) << std::string("symbol not found: ") + name;
        return nullptr;
    }
}

using fn_ft_abi_version        = int(FT_CALL *)();
using fn_ft_free               = void(FT_CALL *)(void *);
using fn_ft_job_result_destroy = void(FT_CALL *)(ft_job_result *);
using fn_ft_job_msg_destroy    = void(FT_CALL *)(ft_job_msg *);

using fn_ft_tunnel_create        = ft_err(FT_CALL *)(const char *url, FT_TunnelHandle **out);
using fn_ft_tunnel_retain        = void(FT_CALL *)(FT_TunnelHandle *);
using fn_ft_tunnel_release       = void(FT_CALL *)(FT_TunnelHandle *);
using fn_ft_tunnel_start_connect = ft_err(FT_CALL *)(FT_TunnelHandle *, void(FT_CALL *)(void *user, int ok, int err, const char *msg), void *user);
using fn_ft_tunnel_sync_connect  = ft_err(FT_CALL *)(FT_TunnelHandle *);
using fn_ft_tunnel_set_status_cb = ft_err(FT_CALL *)(FT_TunnelHandle *, void(FT_CALL *)(void *user, int old_status, int new_status, int err, const char *msg), void *user);
using fn_ft_tunnel_shutdown      = ft_err(FT_CALL *)(FT_TunnelHandle *);

using fn_ft_job_create        = ft_err(FT_CALL *)(const char *params_json, FT_JobHandle **out);
using fn_ft_job_retain        = void(FT_CALL *)(FT_JobHandle *);
using fn_ft_job_release       = void(FT_CALL *)(FT_JobHandle *);
using fn_ft_job_set_result_cb = ft_err(FT_CALL *)(FT_JobHandle *, void(FT_CALL *)(void *user, ft_job_result result), void *user);
using fn_ft_job_get_result    = ft_err(FT_CALL *)(FT_JobHandle *, uint32_t timeout_ms, ft_job_result *out_result);
using fn_ft_tunnel_start_job  = ft_err(FT_CALL *)(FT_TunnelHandle *, FT_JobHandle *);
using fn_ft_job_cancel        = ft_err(FT_CALL *)(FT_JobHandle *);

using fn_ft_job_msg_destroy = void(FT_CALL *)(ft_job_msg *);
using fn_ft_job_set_msg_cb  = ft_err(FT_CALL *)(FT_JobHandle *, void(FT_CALL *)(void *user, ft_job_msg msg), void *user);
using fn_ft_job_try_get_msg = ft_err(FT_CALL *)(FT_JobHandle *, ft_job_msg *out_msg);
using fn_ft_job_get_msg     = ft_err(FT_CALL *)(FT_JobHandle *, uint32_t timeout_ms, ft_job_msg *out_msg);

/// <summary>
/// FileTransferModule, support file operations on printer
/// </summary>
struct FileTransferModule
{
    fn_ft_abi_version        ft_abi_version{};
    fn_ft_free               ft_free{};
    fn_ft_job_result_destroy ft_job_result_destroy{};
    fn_ft_job_msg_destroy    ft_job_msg_destroy{};

    // tunnel(connection)
    fn_ft_tunnel_create        ft_tunnel_create{};
    fn_ft_tunnel_retain        ft_tunnel_retain{};
    fn_ft_tunnel_release       ft_tunnel_release{};
    fn_ft_tunnel_start_connect ft_tunnel_start_connect{};
    fn_ft_tunnel_sync_connect  ft_tunnel_sync_connect{};
    fn_ft_tunnel_set_status_cb ft_tunnel_set_status_cb{};
    fn_ft_tunnel_shutdown      ft_tunnel_shutdown{};

    // job(operation)
    fn_ft_job_create        ft_job_create{};
    fn_ft_job_retain        ft_job_retain{};
    fn_ft_job_release       ft_job_release{};
    fn_ft_job_set_result_cb ft_job_set_result_cb{};
    fn_ft_job_get_result    ft_job_get_result{};
    fn_ft_tunnel_start_job  ft_tunnel_start_job{};
    fn_ft_job_cancel        ft_job_cancel{};

    fn_ft_job_set_msg_cb  ft_job_set_msg_cb{};
    fn_ft_job_try_get_msg ft_job_try_get_msg{};
    fn_ft_job_get_msg     ft_job_get_msg{};

    ModuleHandle networking_{};

    explicit FileTransferModule(ModuleHandle networking_module, int required_abi_version = 1);

    FileTransferModule(const FileTransferModule &)            = delete;
    FileTransferModule &operator=(const FileTransferModule &) = delete;
};

class FileTransferTunnel
{
public:
    using ConnectionCb   = std::function<void(bool is_success, int err_code, std::string error_msg)>;
    using TunnelStatusCb = std::function<void(int old_status, int new_status, int err_code, std::string error_msg)>;

    explicit FileTransferTunnel(FileTransferModule &m, const std::string &url);
    ~FileTransferTunnel() { reset(); }

    FileTransferTunnel(const FileTransferTunnel &)            = delete;
    FileTransferTunnel &operator=(const FileTransferTunnel &) = delete;
    FileTransferTunnel(FileTransferTunnel &&)                 = delete;
    FileTransferTunnel &operator=(FileTransferTunnel &&)      = delete;

    void start_connect();
    bool sync_start_connect();
    void on_connection(ConnectionCb cb);
    void on_status(TunnelStatusCb cb);

    void shutdown();

    int              get_status() const { return status_; }
    bool             check_valid() const { return h_ != nullptr; }
    FT_TunnelHandle *native() const noexcept { return h_; }

private:
    void reset() noexcept
    {
        if (h_) {
            m_->ft_tunnel_release(h_);
            h_ = nullptr;
        }
    }

    int                 status_{};
    FileTransferModule *m_{};
    FT_TunnelHandle    *h_{};
    ConnectionCb        conn_cb_{};
    TunnelStatusCb      status_cb_{};
};

class FileTransferJob
{
public:
    using ResultCb = std::function<void(int res, int resp_ec, std::string json_res, std::vector<std::byte> bin_res)>;
    using MsgCb = std::function<void(int kind, std::string json)>;

    explicit FileTransferJob(FileTransferModule &m, const std::string &params_json);
    ~FileTransferJob() { reset(); }

    FileTransferJob(const FileTransferJob &)            = delete;
    FileTransferJob &operator=(const FileTransferJob &) = delete;
    FileTransferJob(FileTransferJob &&)                 = delete;
    FileTransferJob &operator=(FileTransferJob &&)      = delete;

    void on_result(ResultCb cb);

    bool get_result(int &ec, int &resp_ec, std::string &json, std::vector<std::byte> &bin, uint32_t timeout_ms);

    void start_on(FileTransferTunnel &t);

    void on_msg(MsgCb cb);

    bool try_get_msg(int &kind, std::string &json);

    bool get_msg(uint32_t timeout_ms, int &kind, std::string &json);

    FT_JobHandle *native() const noexcept { return h_; }
    bool          check_valid() const { return h_ != nullptr; }
    bool          finished() const { return finished_; }

    void cancel()
    {
        if (m_->ft_job_cancel && h_) m_->ft_job_cancel(h_);
    }

private:
    void reset() noexcept
    {
        if (h_) {
            m_->ft_job_release(h_);
            h_ = nullptr;
        }
    }

    void solve_result(ft_job_result result);

    FileTransferModule    *m_{};
    FT_JobHandle          *h_{};
    ResultCb               result_cb_{};
    MsgCb                  msg_cb_{};
    bool                   finished_ = false;
    int                    res_      = 0;
    int                    resp_ec_  = 0;
    std::string            res_json_;
    std::vector<std::byte> res_bin_;
};

namespace detail {
inline FileTransferModule *g_mod = nullptr;
}

inline void InitFTModule(ModuleHandle networking_module, int abi_required = 1)
{
    if (detail::g_mod) throw std::runtime_error("Slic3r::InitFTModule called twice");
    detail::g_mod = new FileTransferModule(networking_module, abi_required);
}
inline void UnloadFTModule() noexcept
{
    delete detail::g_mod;
    detail::g_mod = nullptr;
}
inline FileTransferModule &module()
{
    if (!detail::g_mod) throw std::runtime_error("Slic3r::FTModule not initialized. Call Init() first.");
    return *detail::g_mod;
}

} // namespace Slic3r