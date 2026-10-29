#pragma once

// Minimal Result<T> type (std::expected is C++23; we target C++20).
// Every fallible core operation returns a Result so callers cannot
// silently ignore failures.

#include <cassert>
#include <string>
#include <utility>
#include <variant>

namespace acestudio {

enum class ErrorCode {
    Ok = 0,
    InvalidArgument,
    IoError,
    NotFound,
    CorruptData,
    UnsupportedVersion,
    PermissionDenied,
    Cancelled,
    Internal,
};

const char* toString(ErrorCode code) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

    std::string describe() const { return std::string(toString(code)) + ": " + message; }
};

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : m_data(std::move(value)) {}          // NOLINT(implicit)
    Result(Error error) : m_data(std::move(error)) {}      // NOLINT(implicit)

    bool ok() const noexcept { return std::holds_alternative<T>(m_data); }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { assert(ok()); return std::get<T>(m_data); }
    const T& value() const& { assert(ok()); return std::get<T>(m_data); }
    T&& value() && { assert(ok()); return std::get<T>(std::move(m_data)); }

    const Error& error() const { assert(!ok()); return std::get<Error>(m_data); }

private:
    std::variant<T, Error> m_data;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)), m_ok(false) {}  // NOLINT(implicit)

    static Result success() { return {}; }

    bool ok() const noexcept { return m_ok; }
    explicit operator bool() const noexcept { return m_ok; }
    const Error& error() const { assert(!m_ok); return m_error; }

private:
    Error m_error;
    bool m_ok = true;
};

using Status = Result<void>;

} // namespace acestudio
