#pragma once

// ── base::Result<T,E> ───────────────────────────────────────────────────────
// Lightweight stand-in for std::expected (which is C++23) with an API that
// matches the eventual std type closely enough that switching is mechanical.
//
// Design rules:
//   * No exceptions thrown across this boundary.
//   * `value()` on an error result is undefined behavior in release builds
//     and asserts in debug. Callers must check `has_value()` / `operator bool`.
//   * Move-only payloads are supported.
//   * Ok() / Err() helpers mirror common Rust-style construction.
//
// Layer rule: this header lives in `base/` and may not depend on anything
// else in the project.

#include <cassert>
#include <new>
#include <type_traits>
#include <utility>
#include <variant>

namespace daw::base {

template <typename E>
struct Err {
    E value;
};

template <typename E>
Err(E) -> Err<E>;

template <typename T, typename E>
class [[nodiscard]] Result {
public:
    using value_type = T;
    using error_type = E;

    Result(const T& v) : storage_(std::in_place_index<0>, v) {}
    Result(T&& v) : storage_(std::in_place_index<0>, std::move(v)) {}

    Result(Err<E> e) : storage_(std::in_place_index<1>, std::move(e.value)) {}

    bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        assert(has_value() && "Result::value() on error");
        return std::get<0>(storage_);
    }
    const T& value() const& {
        assert(has_value() && "Result::value() on error");
        return std::get<0>(storage_);
    }
    T&& value() && {
        assert(has_value() && "Result::value() on error");
        return std::move(std::get<0>(storage_));
    }

    const E& error() const& {
        assert(!has_value() && "Result::error() on value");
        return std::get<1>(storage_);
    }
    E&& error() && {
        assert(!has_value() && "Result::error() on value");
        return std::move(std::get<1>(storage_));
    }

    template <typename U>
    T value_or(U&& fallback) const& {
        return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
    }

private:
    std::variant<T, E> storage_;
};

// Specialization for void-valued results (operations that can fail but don't
// produce a payload on success).
template <typename E>
class [[nodiscard]] Result<void, E> {
public:
    using value_type = void;
    using error_type = E;

    Result() : has_value_(true) {}
    Result(Err<E> e) : has_value_(false), error_(std::move(e.value)) {}

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    const E& error() const& {
        assert(!has_value_ && "Result::error() on value");
        return error_;
    }
    E&& error() && {
        assert(!has_value_ && "Result::error() on value");
        return std::move(error_);
    }

private:
    bool has_value_;
    E    error_{};
};

template <typename T>
auto Ok(T&& v) {
    return std::forward<T>(v); // implicit conversion at use site
}

inline auto Ok() {
    struct OkVoid {};
    return OkVoid{};
}

} // namespace daw::base
