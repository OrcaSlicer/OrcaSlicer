#ifndef orcaslicer_IPlatformServices_hpp_
#define orcaslicer_IPlatformServices_hpp_

#include <functional>
#include <string>

namespace Slic3r::Portability::Platform {

class IPlatformServices
{
public:
    virtual ~IPlatformServices() = default;

    virtual std::string writable_app_data_path() const = 0;
    virtual std::string temporary_path() const = 0;

    virtual void post_to_main_thread(std::function<void()> task) = 0;
    virtual void post_background(std::function<void()> task) = 0;

    virtual bool read_secure_value(const std::string &key, std::string &value) const = 0;
    virtual bool write_secure_value(const std::string &key, const std::string &value) = 0;
};

} // namespace Slic3r::Portability::Platform

#endif // orcaslicer_IPlatformServices_hpp_
