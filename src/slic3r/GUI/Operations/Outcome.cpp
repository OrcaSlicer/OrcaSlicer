#include "Outcome.hpp"

#include "slic3r/GUI/I18N.hpp"

#include <boost/format.hpp>

#include <string>
#include <vector>

#include <wx/string.h>

namespace Slic3r { namespace GUI { namespace Ops {

namespace {

// `text` with its %1%.. placeholders filled. A message and its values that do not match (a
// translation that drops a placeholder) still give text rather than an exception.
std::string filled(const std::string& text, const std::vector<std::string>& args)
{
    if (args.empty())
        return text;
    boost::format format(text);
    format.exceptions(boost::io::no_error_bits);
    for (const std::string& arg : args)
        format % arg;
    return format.str();
}

} // namespace

wxString Refusal::translated() const { return wxString::FromUTF8(filled(I18N::translate_utf8(msgid), args)); }

std::string Refusal::english() const { return filled(msgid, args); }

}}} // namespace Slic3r::GUI::Ops
