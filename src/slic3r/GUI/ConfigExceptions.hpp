#include <exception> 
namespace Slic3r {

class ConfigError : public Slic3r::RuntimeError { 
	using Slic3r::RuntimeError::RuntimeError;
};

namespace GUI {

class ConfigGUITypeError : public ConfigError { 
	using ConfigError::ConfigError;
};

} // namespace GUI
} // namespace Slic3r
