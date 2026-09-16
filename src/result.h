#pragma once
// Error handling for the indexing pipeline: return values, never exceptions
// (docs/cross/stack.md). A batch job that throws loses track of how far it got.
#include <optional>
#include <string>
#include <utility>
#include <variant>

struct Err {
    std::string msg;
};

template <class T>
class Result {
public:
    Result(T v) : v_(std::move(v)) {}
    Result(Err e) : err_(std::move(e.msg)) {}

    explicit operator bool() const { return v_.has_value(); }
    T& operator*() { return *v_; }
    T* operator->() { return &*v_; }
    const std::string& error() const { return err_; }

private:
    std::optional<T> v_;
    std::string err_;
};

using Status = Result<std::monostate>;
inline constexpr std::monostate ok{};
