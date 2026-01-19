#ifndef slic3r_SimplyPrint_hpp_
#define slic3r_SimplyPrint_hpp_

#include "PrintHost.hpp"
#include "slic3r/GUI/Jobs/OAuthJob.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;
class SimplyPrint : public PrintHost
{
    std::string cred_file;
    std::map<std::string, std::string> cred;

    void load_oauth_credential();

    /**
     * \brief Call the given SimplyPrint API, and if the token expired do a token refresh then retry
     * \param build_request the http request builder
     * \param on_complete 
     * \param on_error 
     * \return whether the API call succeeded
     */
    bool do_api_call(std::function<Http(bool /*is_retry*/)>                                                           build_request,
                     std::function<bool(std::string /* body */, unsigned /* http_status */)>                          on_complete,
                     std::function<bool(std::string /* body */, std::string /* error */, unsigned /* http_status */)> on_error) const;

    /**
     * \brief Upload a temp file and open SimplyPrint panel for file importing
     * \param file_path for file smaller than 100MB, this is the file path, otherwise must left empty
     * \param chunk_id for file greater than 100MB, this is the chunk id returned by the ChunkReceive API, otherwise must left empty
     * \param filename the target file name
     * \param prorgess_fn 
     * \param error_fn 
     * \return whether upload succeeded
     */
    bool do_temp_upload(const boost::filesystem::path& file_path,
                        const std::string&             chunk_id,
                        const std::string&             filename,
                        ProgressFn                     prorgess_fn,
                        ErrorFn                        error_fn) const;

    bool do_chunk_upload(const boost::filesystem::path& file_path,
                         const std::string&             filename,
                         ProgressFn                     prorgess_fn,
                         ErrorFn                        error_fn) const;

public:
    SimplyPrint(DynamicPrintConfig* config);
    ~SimplyPrint() override = default;

    const char* get_name() const override { return "SimplyPrint"; }
    bool can_test() const override { return true; }
    bool has_auto_discovery() const override { return false; }
    bool is_cloud() const override { return true; }
    std::string get_host() const override { return "https://simplyprint.io"; }

    GUI::OAuthParams get_oauth_params() const;
    void             save_oauth_credential(const GUI::OAuthResult& cred) const;

    wxString                   get_test_ok_msg() const override;
    wxString                   get_test_failed_msg(wxString& msg) const override;
    bool                       test(wxString& curl_msg) const override;
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::QueuePrint; }
    bool                       upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
    bool                       is_logged_in() const override { return !cred.empty(); }
    void                       log_out() const override;
};
} // namespace Slic3r

#endif
