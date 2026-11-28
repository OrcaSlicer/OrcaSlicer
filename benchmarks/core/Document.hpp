#pragma once

#include "core/Result.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace Slic3r { namespace Bench {

// Thrown when a result cannot be written as a document, or a document cannot be read as a
// result.
class DocumentError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Writes the result as JSON ending in a newline, with each timeline as offsets from its
// origin().
std::string write_document(const Result& result);

// A suite's start as the document writes it, in UTC, such as "2026-09-28T16:42:07.123Z".
std::string to_iso8601(decltype(Suite::started_at) time);

// Reads what write_document() writes, from any minor version of this schema major, ignoring
// fields it does not know.
Result read_document(std::string_view text);

// Reads the document in the file at `path`, in UTF-8, naming the path in any error.
Result read_document_file(const std::string& path);

}} // namespace Slic3r::Bench
