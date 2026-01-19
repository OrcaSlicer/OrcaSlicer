#ifndef slic3r_MKS_hpp_
#define slic3r_MKS_hpp_

#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"
#include "TCPConsole.hpp"

namespace Slic3r {
class DynamicPrintConfig;
class Http;

class MKS : public PrintHost
{
public:
	explicit MKS(DynamicPrintConfig* config);
	~MKS() override = default;

	const char* get_name() const override;

	bool test(wxString& curl_msg) const override;
	wxString get_test_ok_msg() const override;
	wxString get_test_failed_msg(wxString& msg) const override;
	bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
	bool has_auto_discovery() const override { return false; }
	bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint; }
	std::string get_host() const override { return m_host; }

private:
	std::string m_host;
	std::string m_console_port;

	std::string get_upload_url(const std::string& filename) const;
	bool start_print(wxString& msg, const std::string& filename) const;
	int get_err_code_from_body(const std::string& body) const;
};

}

#endif
