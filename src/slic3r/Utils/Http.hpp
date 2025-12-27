#ifndef __Http_hpp__
#define __Http_hpp__

#include <map>
#include <memory>
#include <string>
#include <functional>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/fstream.hpp>

#include "libslic3r/Exception.hpp"
#include "libslic3r_version.h"

#define MAX_SIZE_TO_FILE    3*1024

namespace Slic3r {

enum HttpErrorCode
{
	HttpErrorResourcesNotFound	= 2,
	HtttErrorNoDevice			= 3,
	HttpErrorRequestLogin		= 4,
	HttpErrorResourcesNotExists = 6,
	HttpErrorMQTTError			= 7,
	HttpErrorResourcesForbidden = 8,
	HttpErrorInternalRequestError = 9,
	HttpErrorInternalError		= 10,
	HttpErrorFileFormatError	= 11,
	HttpErrorResoucesConflict	= 12,
	HttpErrorTimeout			= 13,
	HttpErrorResourcesExhaust   = 14,
	HttpErrorVersionLimited		= 15,
};

/// Represetns a Http request
class Http : public std::enable_shared_from_this<Http> {
private:
	struct priv;
public:
	struct Progress
	{
		size_t dltotal;   // Total bytes to download
		size_t dlnow;     // Bytes downloaded so far
		size_t ultotal;   // Total bytes to upload
		size_t ulnow;     // Bytes uploaded so far
        const std::string& buffer; // reference to buffer containing all data
        double upload_spd{0.0f};

		Progress(size_t dltotal, size_t dlnow, size_t ultotal, size_t ulnow, const std::string& buffer) :
			dltotal(dltotal), dlnow(dlnow), ultotal(ultotal), ulnow(ulnow), buffer(buffer)
		{}

		Progress(size_t dltotal, size_t dlnow, size_t ultotal, size_t ulnow, const std::string& buffer, double ulspd) :
			dltotal(dltotal), dlnow(dlnow), ultotal(ultotal), ulnow(ulnow), buffer(buffer), upload_spd(ulspd)
		{}
	};

	typedef std::shared_ptr<Http> Ptr;
	typedef std::function<void(std::string /* body */, unsigned /* http_status */)> CompleteFn;

	// A HTTP request may fail at various stages of completeness (URL parsing, DNS lookup, TCP connection, ...).
	// If the HTTP request could not be made or failed before completion, the `error` arg contains a description
	// of the error and `http_status` is zero.
	// If the HTTP request was completed but the response HTTP code is >= 400, `error` is empty and `http_status` contains the response code.
	// In either case there may or may not be a body.
	typedef std::function<void(std::string /* body */, std::string /* error */, unsigned /* http_status */)> ErrorFn;

	// See the Progress struct above.
	// Writing true to the `cancel` reference cancels the request in progress.
	typedef std::function<void(Progress, bool& /* cancel */)> ProgressFn;

	typedef std::function<void(std::string/* address */)> IPResolveFn;

	typedef std::function<void(std::string headers)> HeaderCallbackFn;

	Http(Http &&other);

	// Note: strings are expected to be UTF-8-encoded

	// These are the primary constructors that create a HTTP object
	// for a GET and a POST request respectively.
	static Http get(std::string url);
	static Http post(std::string url);
	static Http put(std::string url);
	static Http del(std::string url);

	//BBS
	static Http put2(std::string url);
	static Http patch(std::string url);

	//BBS set global header for each http request
	static void set_extra_headers(std::map<std::string, std::string> headers);
	static std::map<std::string, std::string> get_extra_headers();

	~Http();

	Http(const Http &) = delete;
	Http& operator=(const Http &) = delete;
	Http& operator=(Http &&) = delete;

	// Sets a maximum connection timeout in seconds
	Http& timeout_connect(long timeout);
    // Sets a maximum total request timeout in seconds
    Http& timeout_max(long timeout);
	// Sets a maximum size of the data that can be received.
	// A value of zero sets the default limit, which is is 5MB.
	Http& size_limit(size_t sizeLimit);
	// range  of donloaded bytes. example: curl_easy_setopt(curl, CURLOPT_RANGE, "0-199");
	Http& set_range(const std::string& range);
	// Sets a HTTP header field.
	Http& header(std::string name, const std::string &value);
	// Removes a header field.
	Http& remove_header(std::string name);
	// Authorization by HTTP digest, based on RFC2617.
	Http& auth_digest(const std::string &user, const std::string &password);
    // Basic HTTP authorization
    Http& auth_basic(const std::string &user, const std::string &password);
	// Sets a CA certificate file for usage with HTTPS. This is only supported on some backends,
	// specifically, this is supported with OpenSSL and NOT supported with Windows and OS X native certificate store.
	// See also ca_file_supported().
	Http& ca_file(const std::string &filename);

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

#ifdef WIN32
	// Tells libcurl to ignore certificate revocation checks in case of missing or offline distribution points for those SSL backends where such behavior is present.
	// This option is only supported for Schannel (the native Windows SSL library).
	Http& ssl_revoke_best_effort(bool set);
#endif // WIN32

	// Set the file contents as a POST request body.
	// The data is used verbatim, it is not additionally encoded in any way.
	// This can be used for hosts which do not support multipart requests.
	Http& set_post_body(const boost::filesystem::path &path);

	// Set the POST request body.
	// The data is used verbatim, it is not additionally encoded in any way.
	// This can be used for hosts which do not support multipart requests.
	Http& set_post_body(const std::string &body);

	// Set the file contents as a PUT request body.
	// The data is used verbatim, it is not additionally encoded in any way.
	// This can be used for hosts which do not support multipart requests.
	Http& set_put_body(const boost::filesystem::path &path);

	// Set the file contents as a DELETE request body.
	// The data is used verbatim, it is not additionally encoded in any way.
	// This can be used for hosts which do not support multipart requests.
	Http& set_del_body(const std::string& body);

	// Callback called on HTTP request complete
	Http& on_complete(CompleteFn fn);
	// Callback called on an error occuring at any stage of the requests: Url parsing, DNS lookup,
	// TCP connection, HTTP transfer, and finally also when the response indicates an error (status >= 400).
	// Therefore, a response body may or may not be present.
	Http& on_error(ErrorFn fn);
	// Callback called on data download/upload prorgess (called fairly frequently).
	// See the `Progress` structure for description of the data passed.
	// Writing a true-ish value into the cancel reference parameter cancels the request.
	Http& on_progress(ProgressFn fn);
	// Callback called after succesful HTTP request (after on_complete callback)
	// Called if curl_easy_getinfo resolved just used IP address.
	Http& on_ip_resolve(IPResolveFn fn);
	// Callback called when response header is received
	Http& on_header_callback(HeaderCallbackFn fn);

	// Starts performing the request in a background thread
	Ptr perform();
	// Starts performing the request on the current thread
	void perform_sync();
	// Cancels a request in progress
	void cancel();

	// Tells whether current backend supports seting up a CA file using ca_file()
	static bool ca_file_supported();

    // Return empty string on success or error message on fail.
    static std::string tls_global_init();
    static std::string tls_system_cert_store();

	// converts the given string to an url_encoded_string
	static std::string url_encode(const std::string &str);
	static std::string url_decode(const std::string &str);

	static std::string get_filename_from_url(const std::string &url);
private:
	Http(const std::string &url);

	std::unique_ptr<priv> p;
};

std::ostream& operator<<(std::ostream &, const Http::Progress &);


}

#endif
