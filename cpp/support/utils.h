/*!
 * Copyright (c) 2024 by Contributors
 * \file xgrammar/support/utils.h
 * \brief Utility functions.
 */
#ifndef XGRAMMAR_SUPPORT_UTILS_H_
#define XGRAMMAR_SUPPORT_UTILS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

#include "logging.h"

namespace xgrammar {

/*!
 * \brief Hash and combine value into seed.
 * \ref https://www.boost.org/doc/libs/1_84_0/boost/intrusive/detail/hash_combine.hpp
 */
inline void HashCombineBinary(uint32_t& seed, uint32_t value) {
  seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

/*!
 * \brief Find the hash sum of several uint32_t args.
 */
template <typename... Args>
inline uint32_t HashCombine(Args... args) {
  uint32_t seed = 0;
  (..., HashCombineBinary(seed, args));
  return seed;
}

// Sometimes GCC fails to detect some branches will not return, such as when we use LOG(FATAL)
// to raise an error. This macro manually mark them as unreachable to avoid warnings.
#ifdef __GNUC__
#define XGRAMMAR_UNREACHABLE() __builtin_unreachable()
#else
#define XGRAMMAR_UNREACHABLE()
#endif

/******************* MemorySize Procotol *******************/

template <typename T, typename = std::enable_if_t<std::is_trivially_copyable_v<T>>>
inline constexpr std::size_t MemorySize(const T& value) {
  return 0;
}

/*!
 * \brief Compute the memory consumption in heap memory. This function is specialized for
 * containers.
 * \tparam Container The container type.
 * \param container The container.
 * \return The memory consumption in heap memory of the container.
 */
template <typename T>
inline constexpr std::size_t MemorySize(const std::vector<T>& container) {
  std::size_t result = sizeof(T) * container.size();
  for (const auto& item : container) {
    result += MemorySize(item);
  }
  return result;
}

template <typename T>
inline constexpr std::size_t MemorySize(const std::unordered_set<T>& container) {
  return sizeof(T) * container.size();
}

/*!
 * \brief Compute the memory consumption in heap memory. This function is specialized for
 * std::optional.
 * \tparam Tp The type of the optional.
 * \param range The optional.
 * \return The memory consumption in heap memory of the optional.
 */
template <typename T>
inline constexpr std::size_t MemorySize(const std::optional<T>& optional_value) {
  return optional_value.has_value() ? MemorySize(*optional_value) : 0;
}

/*!
 * \brief An error class that contains a type. The type can be an enum.
 */
template <typename T>
class TypedError : public std::runtime_error {
 public:
  explicit TypedError(T type, const std::string& msg) : std::runtime_error(msg), type_(type) {}
  const T& Type() const noexcept { return type_; }

 private:
  T type_;
};

/*!
 * \brief A Result type similar to Rust's Result, representing either success (Ok) or failure (Err).
 * \tparam T The type of the success value
 */
template <typename T, typename E = std::runtime_error>
class Result {
 private:
  static_assert(!std::is_same_v<T, E>, "T and E cannot be the same type");

 public:
  /*! \brief Construct a success Result with a const reference */
  static Result Ok(const T& value) { return Result(value); }
  /*! \brief Construct a success Result with a move */
  static Result Ok(T&& value) { return Result(std::move(value)); }

  /*! \brief Construct an error Result with a const reference */
  static Result Err(const E& error) { return Result(error); }
  /*! \brief Construct an error Result with a move */
  static Result Err(E&& error) { return Result(std::move(error)); }
  /*! \brief Forward constructor for error type */
  template <typename... Args>
  static Result Err(Args&&... args) {
    return Result(E(std::forward<Args>(args)...));
  }

  /*! \brief Check if Result contains success value */
  bool IsOk() const { return std::holds_alternative<T>(data_); }

  /*! \brief Check if Result contains error */
  bool IsErr() const { return std::holds_alternative<E>(data_); }

  /*! \brief Get the success value, or throw an exception if this is an error */
  T& Unwrap() & {
    if (!IsOk()) {
      XGRAMMAR_LOG(FATAL) << "Called Unwrap() on an Err value";
      XGRAMMAR_UNREACHABLE();
    }
    return std::get<T>(data_);
  }

  /*! \brief Get the success value, or throw an exception if this is an error */
  const T& Unwrap() const& {
    if (!IsOk()) {
      XGRAMMAR_LOG(FATAL) << "Called Unwrap() on an Err value";
      XGRAMMAR_UNREACHABLE();
    }
    return std::get<T>(data_);
  }

  /*! \brief Get the success value, or throw an exception if this is an error */
  T&& Unwrap() && {
    if (!IsOk()) {
      XGRAMMAR_LOG(FATAL) << "Called Unwrap() on an Err value";
      XGRAMMAR_UNREACHABLE();
    }
    return std::move(std::get<T>(data_));
  }

  /*! \brief Get the error value, or throw an exception if this is a success */
  E& UnwrapErr() & {
    if (IsOk()) {
      XGRAMMAR_LOG(FATAL) << "Called UnwrapErr() on an Ok value";
      XGRAMMAR_UNREACHABLE();
    }
    return std::get<E>(data_);
  }

  /*! \brief Get the error value, or throw an exception if this is a success */
  const E& UnwrapErr() const& {
    if (!IsErr()) {
      XGRAMMAR_LOG(FATAL) << "Called UnwrapErr() on an Ok value";
      XGRAMMAR_UNREACHABLE();
    }
    return std::get<E>(data_);
  }

  /*! \brief Get the error value, or throw an exception if this is a success */
  E&& UnwrapErr() && {
    if (!IsErr()) {
      XGRAMMAR_LOG(FATAL) << "Called UnwrapErr() on an Ok value";
      XGRAMMAR_UNREACHABLE();
    }
    return std::move(std::get<E>(data_));
  }

  /*! \brief Get the success value if present, otherwise return the provided default */
  T UnwrapOr(T default_value) const { return IsOk() ? std::get<T>(data_) : default_value; }

  /*! \brief Map success value to new type using provided function */
  template <typename U, typename F>
  Result<U> Map(F&& f) const {
    if (IsOk()) {
      return Result<U, E>::Ok(f(std::get<T>(data_)));
    }
    return Result<U, E>::Err(std::get<E>(data_));
  }

  /*! \brief Map error value to new type using provided function */
  template <typename V, typename F>
  Result<T, V> MapErr(F&& f) const {
    if (IsErr()) {
      return Result<T, V>::Err(f(std::get<E>(data_)));
    }
    return Result<T, V>::Ok(std::get<T>(data_));
  }

 private:
  explicit Result(const T& value) : data_(value) {}
  explicit Result(T&& value) : data_(std::move(value)) {}
  explicit Result(const E& err) : data_(err) {}
  explicit Result(E&& err) : data_(std::move(err)) {}

  std::variant<T, E> data_;
};

}  // namespace xgrammar

namespace std {

template <typename T, typename U>
struct hash<std::pair<T, U>> {
  size_t operator()(const std::pair<T, U>& pair) const {
    return xgrammar::HashCombine(std::hash<T>{}(pair.first), std::hash<U>{}(pair.second));
  }
};

template <typename... Args>
struct hash<std::tuple<Args...>> {
  size_t operator()(const std::tuple<Args...>& tuple) const {
    return std::apply(
        [](const Args&... args) { return xgrammar::HashCombine(std::hash<Args>{}(args)...); }, tuple
    );
  }
};

template <typename T>
struct hash<std::vector<T>> {
  size_t operator()(const std::vector<T>& vec) const {
    uint32_t seed = 0;
    for (const auto& item : vec) {
      xgrammar::HashCombineBinary(seed, std::hash<T>{}(item));
    }
    return seed;
  }
};

}  // namespace std

#endif  // XGRAMMAR_SUPPORT_UTILS_H_
