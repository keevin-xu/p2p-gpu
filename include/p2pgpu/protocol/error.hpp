#pragma once
//
// The error model for the trust boundary (docs/CONVENTIONS.md §1).
//
// ── NO EXCEPTIONS ACROSS THIS BOUNDARY ───────────────────────────────────
// A hostile worker must not be able to steer control flow through our unwind
// paths. Everything that touches network bytes returns Result<T>/Status and
// never throws. Exceptions remain permitted only in startup/config code and in
// tests.
//
// ── WHY A HAND-ROLLED Result RATHER THAN std::expected ───────────────────
// std::expected is C++23. This project is C++20 (CLAUDE.md), and the check was
// run rather than assumed: __cpp_lib_expected is undefined under -std=c++20 on
// both Apple Clang 17 and Emscripten 6.0.5, and defined only at -std=c++23.
// CONVENTIONS.md §1 anticipated this ("or a project Result<T> alias").
//
// The interface is deliberately a subset of std::expected's, so switching to
// the standard type later is a using-declaration and not a rewrite.

#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

#include "p2pgpu/p2pgpu_generated.h"

namespace p2pgpu::protocol {

/// Reuses the schema's ErrorCode rather than declaring a parallel enum: these
/// values go on the wire in `Error.code`, and two definitions of the same
/// vocabulary is a drift bug waiting to happen (R3).
using ErrorCode = wire::ErrorCode;

struct Error {
    ErrorCode code = ErrorCode::Internal;

    /// Static text only. A string_view here never owns, so it must point at a
    /// literal — never at a buffer derived from network input, which would be
    /// a dangling read the moment the frame is recycled.
    std::string_view message{};

    /// `true` => the peer must not retry with the same state. A version
    /// mismatch is fatal; a malformed frame is not (docs/PROTOCOL.md §3).
    bool fatal = false;
};

[[nodiscard]] constexpr Error MakeError(ErrorCode code, std::string_view message,
                                        bool fatal = false) noexcept {
    return Error{code, message, fatal};
}

/// Success-or-Error, with no value. `Status` reads better than `Result<void>`
/// and avoids specialising the template.
class [[nodiscard]] Status {
public:
    constexpr Status() noexcept = default;                       // success
    constexpr Status(Error err) noexcept : err_(err), ok_(false) {}  // NOLINT: implicit by design

    [[nodiscard]] constexpr bool ok() const noexcept { return ok_; }
    constexpr explicit operator bool() const noexcept { return ok_; }
    [[nodiscard]] constexpr const Error& error() const noexcept { return err_; }

private:
    Error err_{};
    bool ok_ = true;
};

/// A value or an Error. Deliberately a subset of std::expected.
template <typename T>
class [[nodiscard]] Result {
public:
    constexpr Result(T value) noexcept(std::is_nothrow_move_constructible_v<T>)  // NOLINT
        : v_(std::move(value)) {}
    constexpr Result(Error err) noexcept : v_(err) {}  // NOLINT: implicit by design

    [[nodiscard]] constexpr bool has_value() const noexcept { return v_.index() == 0; }
    constexpr explicit operator bool() const noexcept { return has_value(); }

    // Precondition: has_value(). std::get enforces it by throwing, which is
    // acceptable here because reaching it is a PROGRAMMING error in our own
    // code, never something a hostile peer can trigger — the peer's influence
    // ends at whether the Result holds an error, and checking that is the
    // caller's obligation.
    [[nodiscard]] constexpr const T& operator*() const& { return std::get<0>(v_); }
    [[nodiscard]] constexpr const T* operator->() const { return &std::get<0>(v_); }

    /// Rvalue overload, so a MOVE-ONLY `T` can be taken out.
    ///
    /// Added at 2.19, when the first `Result<std::unique_ptr<...>>` appeared and
    /// there was no way to extract it — every existing use held a copyable type,
    /// so the gap was invisible. Ref-qualified (`&&`) rather than added as a
    /// plain non-const overload: moving out of a named `Result` must be spelled
    /// `std::move(r)` at the call site, or a `*r` in a loop would silently
    /// hollow out the value on the first iteration.
    [[nodiscard]] constexpr T&& operator*() && { return std::get<0>(std::move(v_)); }

    [[nodiscard]] constexpr const Error& error() const { return std::get<1>(v_); }

private:
    std::variant<T, Error> v_;
};

}  // namespace p2pgpu::protocol
