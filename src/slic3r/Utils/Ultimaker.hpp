#ifndef slic3r_Ultimaker_hpp_
#define slic3r_Ultimaker_hpp_

#include <string>
#include <wx/string.h>

#include "PrintHost.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Http;

class Ultimaker : public PrintHost
{
public:
    explicit Ultimaker(DynamicPrintConfig *config);
	~Ultimaker() override = default;

	const char* get_name() const override;

	bool test(wxString &curl_msg) const override;
	wxString get_test_ok_msg() const override;
	wxString get_test_failed_msg(wxString &msg) const override;

	bool has_auth_creds() const;
	bool is_authorized() const;
	std::string auth_status() const;
	std::string generate_auth_creds() const;
	std::string test_auth() const;

	bool upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const override;
	bool has_auto_discovery() const override { return false; }
	bool can_test() const override { return true; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return PrintHostPostUploadAction::StartPrint | PrintHostPostUploadAction::StartSimulation; }
	std::string get_host() const override { return host; }
	const std::string& get_api_user() const { return m_api_username; }
	const std::string& get_api_username() const { return m_api_username; }
	const std::string& get_api_password() const { return m_api_password; }

protected:
	std::string m_host;
	
    // username and password for HTTP Digest Authentization (RFC RFC2617)
	std::string m_api_username;
    std::string m_api_password;

	bool makeGriffinCompatible(std::string filepath) const;
   
private:
	enum class ConnectionType { rrf, dsf, error };
	std::string host;

	std::string get_upload_url(const std::string &filename, ConnectionType connectionType) const;
	std::string get_status_url() const;
	std::string get_connect_url(const bool dsfUrl) const;
	std::string get_base_url() const;
	std::string timestamp_str() const;
	ConnectionType connect(wxString &msg) const;
	void set_auth(Http& http) const;
	void disconnect(ConnectionType connectionType) const;
	bool start_print(wxString &msg, const std::string &filename, ConnectionType connectionType, bool simulationMode) const;
	int get_err_code_from_body(const std::string &body) const;
};

}

#endif
