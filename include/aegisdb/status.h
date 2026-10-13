#pragma once
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace aegisdb {

enum class Code {
    ok,
    not_found,
    invalid_argument,
    io_error,
    corruption,
    resource_exhausted,
    timeout,
    busy,
    shutting_down,
    internal_error
};

struct Status {
    Code code{Code::ok};
    std::string message;

    [[nodiscard]] // causes warning if the return value is ignored.
    bool ok() const noexcept { return code == Code::ok; }

    explicit operator bool() const noexcept { return ok(); } // explicit operator to use object as the function ..

    static Status OK() { return {}; }
};

template <class T> class Result {
  public:
  
    Result(T value) : data_(std::move(value)) {}

    Result(Status error) : data_(std::move(error)) {
        if (std::get<Status>(data_).ok())
            throw std::logic_error("Result error must not be OK");
    }

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(data_); }

    explicit operator bool() const noexcept { return ok(); }

    const Status &status() const {
        static const Status success;
        return ok() ? success : std::get<Status>(data_);
    }

    T &value() & { return std::get<T>(data_); }

    const T &value() const & { return std::get<T>(data_); }

    T &&value() && { return std::get<T>(std::move(data_)); }

  private:
    std::variant<T, Status> data_;
};

namespace detail {
class Error : public std::runtime_error {
  public:
    Error(Code code, std::string message)
        : std::runtime_error(message), status{code, std::move(message)} {}
    Status status;
};
inline void require(bool condition, Code code, const char *message) {
    if (!condition)
        throw Error(code, message);
}
Status exceptionStatus() noexcept;
} // namespace detail
} // namespace aegisdb
