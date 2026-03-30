#include "UltiMaker.hpp"

#include <algorithm>
#include <ctime>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <wx/frame.h>
#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "Http.hpp"

#include <iostream>
#include <stdio.h>
#include <string>
namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

UltiMaker::UltiMaker(DynamicPrintConfig *config) :
	
	m_host(config->opt_string("print_host")),
	host(config->opt_string("print_host")),

    m_api_username(config->opt_string("printhost_user")),
	m_api_password(config->opt_string("printhost_password"))
{}

const char* UltiMaker::get_name() const { return "UltiMaker"; }

// Modified from OctoPrint::test in OctoPrint.cpp
bool UltiMaker::test(wxString &msg) const
{

	// If called with specific arg, just generate the auth creds.
	if (msg == "generate_auth_creds") {
		BOOST_LOG_TRIVIAL(warning) << "UltiMaker: test called with generate_auth_creds! Generating the auth credentials.";
		this->generate_auth_creds();
		return true;
	}

	// Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
	BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: Attempting to test machine");

    const char* name = get_name();

    bool res = true;
    auto url = (boost::format("http://%1%/api/v1/system/variant") % host).str();

	BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: name %1% url %2%") % name % url;
    BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Get system variant at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting system variant: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
        res = false;
        msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: url completed without error: %1%") % url;
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Got system variant: %2%") % name % body;
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: res=%2%") % name % res; // DEBUG

            try {
				// Validate that response is correct ("UltiMaker 3", "UltiMaker 3 extended" or "UltiMaker S5")
				res = (boost::starts_with(body, "\"UltiMaker 3") || body == "\"UltiMaker S5\"");
            }
            catch (const std::exception &) {
                res = false;
                msg = "Could not parse server response";
				BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Caught error") % name;
				// BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Caught error: %2%") % name % e;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();


	BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: res=%2%") % name % res; //DEBUG
    BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Now doing authentication test.") % name;
	std::string auth_result = test_auth();
	if ( not(boost::starts_with(auth_result,"OK")) ) { msg = auth_result; res = false; };
    BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Auth test result: %2%") % name % auth_result;
	
    return res;
}


wxString UltiMaker::get_test_ok_msg () const
{
	return _("Connection to UltiMaker works correctly.");
}

wxString UltiMaker::get_test_failed_msg (wxString &msg) const
{
    return GUI::from_u8((boost::format("%s: %s")
                    // % _utf8(L("Could not connect to UltiMaker"))
                    % _utf8("Could not connect to UltiMaker")
                    % std::string(msg.ToUTF8())).str());
}


#pragma region Auth
// Modified from PrusaLink::set_auth in OctoPrint.cpp
void UltiMaker::set_auth(Http& http) const
{
    BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: set_auth, m_api_username of %1% and m_api_password of %2%") % get_api_username() % get_api_password();
    http.auth_digest(m_api_username, m_api_password);
}


bool UltiMaker::has_auth_creds() const{
	// True if has authentication credentials, false otherwise
	return not(m_api_username.empty()) && not(m_api_password.empty());
}


bool UltiMaker::is_authorized() const{

	BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: is_authorized() attempting to see if creds are valid.");

    const char* name = get_name();

    bool rtn = false;
    auto url = (boost::format("http://%1%/api/v1/auth/verify") % host).str();

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error verifying credentials: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;

		if (status == 403 || body == "{\"message\": \"Authorization required.\"}") {
			// TODO: add logic for returning err to user dialog msg
		}

        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: url completed without error: %1%") % url;
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Got credential verification: %2%") % name % body;
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: [auth] rtn=%2%") % name % rtn; // DEBUG

            try {
				// Validate that response is correct
				rtn = (boost::starts_with(body, "\"ok\"") || body == "{\"message\": \"ok\"}");
            }
            catch (const std::exception &) {
                rtn = false;
				BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Caught error") % name;
				// BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Caught error: %2%") % name % e;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            // msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();
	
	BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: [auth] rtn=%2%") % name % rtn; //DEBUG
	return rtn;
}




std::string UltiMaker::auth_status() const{
	// returns 'authorized', 'unauthorized', or 'waiting', or 'ERROR: unknown value' on a different output

	BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: auth_status() attempting to see if creds are valid.");

    const char* name = get_name();

	std::string rtn = "ERROR: unknown value";
    auto url = (boost::format("http://%1%/api/v1/auth/check/%2%") % host % m_api_username).str();

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error verifying credentials: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;

        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: url completed without error: %1%") % url;
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Got credential verification: %2%") % name % body;

            try {
				if (body == "{\"message\": \"unknown\"}") { rtn = "waiting";}
				if (body == "{\"message\": \"authorized\"}"  ) {rtn = "authorized";}
				if (body == "{\"message\": \"unauthorized\"}") {rtn = "unauthorized";}

            }
            catch (const std::exception &) {
				BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Caught error") % name;
				// BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Caught error: %2%") % name % e;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            // msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();
	
	if (boost::starts_with(rtn, "ERROR")) { BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: [auth][auth_status] Unknown value encountered inn api/v1/auth/check") % name;}
	return rtn;
}





std::string UltiMaker::generate_auth_creds() const {
	//TODO: Implement.
	// Send POST request to generate creds
	// Get the ID and key from the request
	// Wait for user to authorize on the physical machine
	// Return the result.
	BOOST_LOG_TRIVIAL(warning) << "UltiMaker: generate_auth_creds called!";
	return "Unimplemeneted.";
}



std::string UltiMaker::test_auth() const {
	/* 
	Used in UltiMaker::test(), creates creds if they don't already
	exist, and returns various error strings if errors.
	Returns "OK" if no errors and creds are valid.
	*/
	const char* name = get_name();
	BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Testing for presence of auth creds...") % name;

	// Auth credentials are alreay existing (user-entered)
	if (has_auth_creds()) {
		BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Has auth credentials. Testing validity...") % name;

		std::string auth_stat = auth_status();
		BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: auth_status returned %2%") % name % auth_stat;

		if (auth_stat == "waiting") { 
			BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Auth credentials are waiting for user approval at the physical machine.") % name;
			return "Authentication creds waiting on approval; Please approve access via the dialog box on the printer's screen.";

		} else if (auth_stat == "authorized" && is_authorized()) {
			BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Auth credentials are valid. Returning OK") % name;
			return "OK";

		} else {
			BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Auth credentials are INVALID. auth_status returned %2%") % name % auth_stat;
			return "Invalid authentication credentials";
		}

	} else { // Auth credentials do not exist
		BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Auth credentials do NOT already exist.") % name;
		BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: TODO: create generateAuthCreds logic.") % name;
		//TODO create the auth creds
		// generateAuthCreds(); // NOT a cost member function, what to do? Add another button?

		return "Error: No authentication credentials found!";
	}

}

#pragma endregion Auth


int UltiMaker::getPrintTime(std::string filepath) const {
	// Get the total print time in seconds from the provided gcode filepath
	// TODO: find a way to read the other orca variables without iterating another time through the gcode
	std::ifstream file_in(filepath);
	std::string line;
	int ret = 0;
	int h = 0;
	int m = 0;
	int s = 0;

	if (file_in.is_open()){
		while (getline(file_in, line) && ret == 0) {
			if (boost::starts_with(line, "; estimated printing time (normal mode) =")) {
				if (sscanf(line.c_str(), "; estimated printing time (normal mode) = %dh %dm %ds", &h, &m, &s) >= 1) {
					ret = 3600*h + 60*m + s;
				} else {
					BOOST_LOG_TRIVIAL(error) << boost::format("UltiMaker: ERROR: sscanf error while getting total print time, no vars assigned.");
				}
			}
		}
    } else {
		BOOST_LOG_TRIVIAL(error) << boost::format("UltiMaker: ERROR: file %1% or file %2% was unable to open!") % filepath % (filepath+".temp");
		return false;
	}

	file_in.close();

	return ret;
}


bool UltiMaker::makeGriffinCompatible(std::string filepath) const {
	// Modifies the header of the given file path to remove the double colon
	// at the timestamp because - if left in - it makes UltiMakers crash.

	// Opening and modifying files code was modified from https://www.w3resource.com/cpp-exercises/file-handling/cpp-file-handling-exercise-6.php
	std::string file_temp = filepath+".temp";
	std::ifstream file_in(filepath);
	std::ofstream file_out(file_temp);
	std::string line;
	bool write_on = false;

	if (file_in.is_open() && file_out.is_open()){
		while (getline(file_in, line)) {

			if (write_on) {
				if (boost::starts_with(line, "; generated by OrcaSlicer")) {
					boost::replace_all(line, ":", "-"); // Replace the colons in the date with dashes to prevent printer crash.
				}

				// UM machines need total print time in seconds.
				if (boost::starts_with(line, ";PRINT.TIME:")) {
					file_out << ";PRINT.TIME:"<< getPrintTime(filepath) << "\n";
				
				} else {	
					file_out << line << "\n"; // Write the modified line to the output file
				}
				// TODO: fix potential crashes by replacing additional colons with dashes. (detect lines with more than one colon)

			} else if (boost::starts_with(line, ";START_OF_HEADER")) {
				write_on = true;
				file_out << line << "\n";
			}
    	}
	} else {
		BOOST_LOG_TRIVIAL(error) << boost::format("UltiMaker: ERROR: file %1% or file %2% was unable to open!") % filepath % (filepath+".temp");
		return false;
	}

	if (not(write_on)) {
		BOOST_LOG_TRIVIAL(error) << boost::format("UltiMaker: ERROR: while postprocessing to make compatible with Griffin, headers not found!");
	}

	file_in.close();
	file_out.close();

	// Now replace the upload file with the temp one
	std::remove(filepath.c_str());
	// std::rename(file_temp.c_str(), filepath.c_str()); // TODO: uncomment this
	boost::filesystem::copy_file(file_temp, filepath);
	return true;
}


bool UltiMaker::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
	/* 
		TODO: delete this reference.
		Reference to Http.hpp
		Http& form_clear();
		// Add a HTTP multipart form field
		Http& form_add(const std::string &name, const std::string &contents);
		// Add a HTTP multipart form file data contents, `name` is the name of the part
		Http& form_add_file(const std::string &name, const boost::filesystem::path &path, boost::filesystem::ifstream::off_type offset = 0, size_t length = 0);
		// Add a HTTP mime form field
		Http& mime_form_add_text(std::string& name, std::string& value);
		// Add a HTTP mime form file
		Http& mime_form_add_file(std::string& name, const char* path);
		// Same as above except also override the file's filename with a wstring type
		Http& form_add_file(const std::wstring& name, const boost::filesystem::path& path, boost::filesystem::ifstream::off_type offset = 0, size_t length = 0);
		// Same as above except also override the file's filename with a custom one
		Http& form_add_file(const std::string &name, const boost::filesystem::path &path, const std::string &filename, boost::filesystem::ifstream::off_type offset = 0, size_t length = 0);
	
		jobname
		file
		owner
		created_at
	
		*/

	// Preprocess the file so the machine doesn't crash
	if (makeGriffinCompatible(upload_data.source_path.string())) {
		BOOST_LOG_TRIVIAL(warning) << "UltiMaker: [upload] Griffin compatibility appeared successful.";
	} else {
		BOOST_LOG_TRIVIAL(warning) << "UltiMaker: [upload] Griffin compatibility FAILED.";
		return false;
	}
	
	wxString connect_msg;
	auto connectionType = connect(connect_msg);
	if (connectionType == ConnectionType::error) {
		BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: [upload] connectionType is of type error!");
		error_fn(std::move(connect_msg));
		return false;
	}

	bool res = true;
	bool dsf = (connectionType == ConnectionType::dsf);

	auto upload_cmd = get_upload_url(upload_data.upload_path.string(), connectionType);
	BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: Uploading file %1%, filepath: %2%, post_action: %3%, command: %4%")
		% upload_data.source_path
		% upload_data.upload_path
		% int(upload_data.post_action)
		% upload_cmd;

	auto http = (dsf ? Http::put(std::move(upload_cmd)) : Http::post(std::move(upload_cmd)));
	// if (dsf) {
	// 	http.set_put_body(upload_data.source_path);
	// } else {
	// 	http.set_post_body(upload_data.source_path);
	// }
	http.form_add("jobname",upload_data.upload_path.string());
	http.form_add_file("file", upload_data.source_path, upload_data.upload_path.string());
	// http.form_add("owner","OrcaSlicer"); // TODO: remove this if unneeded
	// http.form_add("created_at",???); // TODO: fill out

	set_auth(http);
	http.on_complete([&](std::string body, unsigned status) {
			BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: File uploaded: HTTP %1%: %2%") % status % body;

			int err_code = dsf ? (status == 201 ? 0 : 1) : get_err_code_from_body(body);
			if (err_code != 0) {
				BOOST_LOG_TRIVIAL(error) << boost::format("UltiMaker: Request completed but error code was received: %1%") % err_code;
				error_fn(format_error(body, L("Unknown error occurred"), 0));
				res = false;
			} else if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
				wxString errormsg;
				res = start_print(errormsg, upload_data.upload_path.string(), connectionType, false);
				if (! res) {
					error_fn(std::move(errormsg));
				}
			} else if (upload_data.post_action == PrintHostPostUploadAction::StartSimulation) {
				wxString errormsg;
				res = start_print(errormsg, upload_data.upload_path.string(), connectionType, true);
				if (! res) {
					error_fn(std::move(errormsg));
				}
			}
		})
		.on_error([&](std::string body, std::string error, unsigned status) {
			BOOST_LOG_TRIVIAL(error) << boost::format("UltiMaker: Error uploading file: %1%, HTTP %2%, body: `%3%`") % error % status % body;
			error_fn(format_error(body, error, status));
			res = false;
		})
		.on_progress([&](Http::Progress progress, bool &cancel) {
			prorgess_fn(std::move(progress), cancel);
			if (cancel) {
				// Upload was canceled
				BOOST_LOG_TRIVIAL(warning) << "UltiMaker: Upload canceled";
				res = false;
			}
		})
		.perform_sync();

	disconnect(connectionType);

	return res;
}

UltiMaker::ConnectionType UltiMaker::connect(wxString &msg) const
{
	BOOST_LOG_TRIVIAL(warning) << "UltiMaker: Attempting to connect"; // TODO: Remove this. Testing to see if var substitution is the issue
	BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: Attempting to connect"); // TODO: Remove this. Testing to see if var substitution is the issue
	
	auto res = ConnectionType::error;
	auto url = get_connect_url(false);

	auto http = Http::get(std::move(url));
	set_auth(http);

	BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: Attempting to connect to url %1%") % url;

	http.on_error([&](std::string body, std::string error, unsigned status) {
			auto dsfUrl = get_connect_url(true);
			auto dsfHttp = Http::get(std::move(dsfUrl));
			dsfHttp.on_error([&](std::string body, std::string error, unsigned status) {
					BOOST_LOG_TRIVIAL(error) << boost::format("UltiMaker: Error connecting: %1%, HTTP %2%, body: `%3%`") % error % status % body;
					msg = format_error(body, error, status);
				})
				.on_complete([&](std::string body, unsigned) {
					res = ConnectionType::dsf;
				})
				.perform_sync();
		})
		.on_complete([&](std::string body, unsigned) {
			BOOST_LOG_TRIVIAL(warning) << boost::format("UltiMaker: Got: %1%") % body;

			int err_code = get_err_code_from_body(body);
			switch (err_code) {
				case 0:
					res = ConnectionType::rrf;
					break;
				case 1:
					msg = format_error(body, L("Wrong password"), 0);
					break;
				case 2:
					msg = format_error(body, L("Could not get resources to create a new connection"), 0);
					break;
				default:
					msg = format_error(body, L("Unknown error occurred"), 0);
					break;
			}

		})
		.perform_sync();

	return res;
}

void UltiMaker::disconnect(ConnectionType connectionType) const
{
	// we don't need to disconnect from DSF or if it failed anyway
	if (connectionType != ConnectionType::rrf) {
		return;
	}
	auto url =  (boost::format("%1%rr_disconnect")
			% get_base_url()).str();

	auto http = Http::get(std::move(url));
	http.on_error([&](std::string body, std::string error, unsigned status) {
		// we don't care about it, if disconnect is not working UltiMaker will disconnect automatically after some time
		BOOST_LOG_TRIVIAL(error) << boost::format("UltiMaker: Error disconnecting: %1%, HTTP %2%, body: `%3%`") % error % status % body;
	})
	.perform_sync();
}


#pragma region Get Constants
std::string UltiMaker::get_upload_url(const std::string &filename, ConnectionType connectionType) const
{
	// Fixed?
	// TODO: figure out what connectionType even is.
    assert(connectionType != ConnectionType::error);

	if (connectionType == ConnectionType::dsf) {
		return (boost::format("%1%/machine/file/gcodes/%2%")
				% get_base_url()
				% Http::url_encode(filename)).str();
	} else {
		return get_base_url()+"/print_job";
				// % Http::url_encode(filename)
				// % timestamp_str()).str();
			// );
	}
}

std::string UltiMaker::get_status_url() const
{
	return (boost::format("%1%/printer/status") % get_base_url()).str();
}

std::string UltiMaker::get_connect_url(const bool dsfUrl) const
{
	if (dsfUrl)	{
		return (boost::format("%1%/printer/status")
				% get_base_url()).str();
	} else {
		return (boost::format("%1%/printer/status")
				% get_base_url()
				// % Http::url_encode(password.empty() ? "reprap" : password) // url_encode is needed because password can contain special characters like `&`, "#", etc.
				// % timestamp_str()
				).str();
	}
}

std::string UltiMaker::get_base_url() const
{
	if (host.find("http://") == 0 || host.find("https://") == 0) {
		if (host.back() == '/') {
			return host;
		} else {
			return (boost::format("%1%/api/v1") % host).str();
		}
	} else {
		return (boost::format("http://%1%/api/v1") % host).str();
	}
}

std::string UltiMaker::timestamp_str() const
{
	enum { BUFFER_SIZE = 32 };

	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);

	char buffer[BUFFER_SIZE];
	std::strftime(buffer, BUFFER_SIZE, "time=%Y-%m-%dT%H:%M:%S", &tm);

	return std::string(buffer);
}

#pragma endregion Get Constants

bool UltiMaker::start_print(wxString &msg, const std::string &filename, ConnectionType connectionType, bool simulationMode) const
{
	// TODO: Fix
    assert(connectionType != ConnectionType::error);

	bool res = false;
	bool dsf = (connectionType == ConnectionType::dsf);

	auto url = dsf
		? (boost::format("%1%machine/code")
			% get_base_url()).str()
		: (boost::format(simulationMode
				? "%1%rr_gcode?gcode=M37%%20P\"0:/gcodes/%2%\""
				: "%1%rr_gcode?gcode=M32%%20\"0:/gcodes/%2%\"")
			% get_base_url()
			% Http::url_encode(filename)).str();

	auto http = (dsf ? Http::post(std::move(url)) : Http::get(std::move(url)));
	set_auth(http);
	if (dsf) {
		http.set_post_body(
				(boost::format(simulationMode
						? "M37 P\"0:/gcodes/%1%\""
						: "M32 \"0:/gcodes/%1%\"")
					% filename).str()
				);
	}
	http.on_error([&](std::string body, std::string error, unsigned status) {
			BOOST_LOG_TRIVIAL(error) << boost::format("UltiMaker: Error starting print: %1%, HTTP %2%, body: `%3%`") % error % status % body;
			msg = format_error(body, error, status);
		})
		.on_complete([&](std::string body, unsigned) {
			BOOST_LOG_TRIVIAL(debug) << boost::format("UltiMaker: Got: %1%") % body;
			res = true;
		})
		.perform_sync();

	return res;
}

int UltiMaker::get_err_code_from_body(const std::string &body) const
{
	pt::ptree root;
	std::istringstream iss (body); // wrap returned json to istringstream
	pt::read_json(iss, root);

	return root.get<int>("err", 0);
}

}
