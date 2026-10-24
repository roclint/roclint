#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace roclint::core {

enum class ErrorCode {
    StaleVersion,
    InvalidInput,
    DbError,
    NotFound,
    Conflict,
    ProviderUnavailable,
    ProviderFailed,
    ValidationFailed,
    PreconditionFailed,
    Internal
};

[[nodiscard]] const char* toString(ErrorCode code) noexcept;

struct Error {
    ErrorCode code{ErrorCode::Internal};
    std::string message{};
};

[[nodiscard]] inline Error makeError(ErrorCode code, std::string message) {
    return Error{code, std::move(message)};
}

template <typename T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const { return std::get<1>(storage_); }

private:
    std::variant<T, Error> storage_;
};

class [[nodiscard]] Status {
public:
    Status() = default;
    Status(Error error) : error_(std::move(error)) {}

    [[nodiscard]] static Status success() { return Status{}; }

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const Error& error() const { return *error_; }

private:
    std::optional<Error> error_{};
};

} // namespace roclint::core
