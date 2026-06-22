#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace orca {

enum class ErrorCode : int {
    Unknown = 0,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    IoError,
    ParseError,
    Cancelled,
    Unsupported,
    NotImplemented,    // method declared but body not yet written (migration scaffold)
    InvalidState,      // engine state precondition not met (e.g. no PresetBundle attached)
    PermissionDenied,  // capability bit missing from the plugin's permission set
};

struct Error {
    ErrorCode   code    = ErrorCode::Unknown;
    std::string message;
};

template <class T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error err) : data_(std::move(err)) {}

    bool ok() const noexcept { return std::holds_alternative<T>(data_); }
    explicit operator bool() const noexcept { return ok(); }

    T&       value() &       { return std::get<T>(data_); }
    const T& value() const & { return std::get<T>(data_); }
    T&&      value() &&      { return std::move(std::get<T>(data_)); }

    const Error& error() const & { return std::get<Error>(data_); }
    Error&&      error() &&      { return std::move(std::get<Error>(data_)); }

private:
    std::variant<T, Error> data_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Error err) : error_(std::move(err)) {}

    bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    const Error& error() const & { return *error_; }
    Error&&      error() &&      { return std::move(*error_); }

private:
    std::optional<Error> error_;
};

inline Result<void> ok() { return Result<void>{}; }

template <class T>
Result<T> ok(T value) { return Result<T>{std::move(value)}; }

inline Error err(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

// Typed convenience: build a Result<T> already in the error state.
template <class T>
Result<T> err(ErrorCode code, std::string message) {
    return Result<T>{Error{code, std::move(message)}};
}

template <>
inline Result<void> err<void>(ErrorCode code, std::string message) {
    return Result<void>{Error{code, std::move(message)}};
}

} // namespace orca
