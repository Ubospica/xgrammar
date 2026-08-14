/*!
 *  Copyright (c) 2026 by Contributors
 * \file xgrammar/support/signed_decimal.h
 * \brief Small exact signed-decimal-integer helpers used for JSON-number exponents.
 */

#ifndef XGRAMMAR_SUPPORT_SIGNED_DECIMAL_H_
#define XGRAMMAR_SUPPORT_SIGNED_DECIMAL_H_

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace xgrammar {

/*! \brief A canonical arbitrary-precision signed integer in base 10. */
struct SignedDecimalInteger {
  bool negative = false;
  std::string digits = "0";
};

inline SignedDecimalInteger MakeSignedDecimalInteger(bool negative, std::string digits) {
  const size_t first_nonzero = digits.find_first_not_of('0');
  if (first_nonzero == std::string::npos) {
    return {};
  }
  digits.erase(0, first_nonzero);
  return {negative, std::move(digits)};
}

inline SignedDecimalInteger MakeSignedDecimalInteger(bool negative, std::string_view digits) {
  return MakeSignedDecimalInteger(negative, std::string(digits));
}

inline SignedDecimalInteger MakeSignedDecimalInteger(int64_t value) {
  if (value >= 0) {
    return {false, std::to_string(value)};
  }
  // Avoid negating INT64_MIN in its signed type.
  const uint64_t magnitude = static_cast<uint64_t>(-(value + 1)) + 1;
  return {true, std::to_string(magnitude)};
}

inline int CompareUnsignedDecimalDigits(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) {
    return lhs.size() < rhs.size() ? -1 : 1;
  }
  if (lhs == rhs) {
    return 0;
  }
  return lhs < rhs ? -1 : 1;
}

inline int CompareSignedDecimalInteger(
    const SignedDecimalInteger& lhs, const SignedDecimalInteger& rhs
) {
  if (lhs.negative != rhs.negative) {
    return lhs.negative ? -1 : 1;
  }
  const int magnitude_compare = CompareUnsignedDecimalDigits(lhs.digits, rhs.digits);
  return lhs.negative ? -magnitude_compare : magnitude_compare;
}

inline std::string AddUnsignedDecimalDigits(std::string_view lhs, std::string_view rhs) {
  std::string result;
  result.reserve(std::max(lhs.size(), rhs.size()) + 1);
  int64_t lhs_index = static_cast<int64_t>(lhs.size()) - 1;
  int64_t rhs_index = static_cast<int64_t>(rhs.size()) - 1;
  int carry = 0;
  while (lhs_index >= 0 || rhs_index >= 0 || carry != 0) {
    int sum = carry;
    if (lhs_index >= 0) {
      sum += lhs[lhs_index--] - '0';
    }
    if (rhs_index >= 0) {
      sum += rhs[rhs_index--] - '0';
    }
    result.push_back(static_cast<char>('0' + sum % 10));
    carry = sum / 10;
  }
  std::reverse(result.begin(), result.end());
  return result;
}

/*! \brief Return lhs - rhs for canonical unsigned decimals with lhs >= rhs. */
inline std::string SubtractUnsignedDecimalDigits(std::string_view lhs, std::string_view rhs) {
  std::string result(lhs);
  int64_t lhs_index = static_cast<int64_t>(result.size()) - 1;
  int64_t rhs_index = static_cast<int64_t>(rhs.size()) - 1;
  int borrow = 0;
  while (lhs_index >= 0) {
    int difference = (result[lhs_index] - '0') - borrow;
    if (rhs_index >= 0) {
      difference -= rhs[rhs_index--] - '0';
    }
    if (difference < 0) {
      difference += 10;
      borrow = 1;
    } else {
      borrow = 0;
    }
    result[lhs_index--] = static_cast<char>('0' + difference);
  }
  const size_t first_nonzero = result.find_first_not_of('0');
  return first_nonzero == std::string::npos ? "0" : result.substr(first_nonzero);
}

inline SignedDecimalInteger AddSignedDecimalInteger(
    const SignedDecimalInteger& lhs, const SignedDecimalInteger& rhs
) {
  if (lhs.negative == rhs.negative) {
    return {lhs.negative, AddUnsignedDecimalDigits(lhs.digits, rhs.digits)};
  }
  const int magnitude_compare = CompareUnsignedDecimalDigits(lhs.digits, rhs.digits);
  if (magnitude_compare == 0) {
    return {};
  }
  if (magnitude_compare > 0) {
    return {lhs.negative, SubtractUnsignedDecimalDigits(lhs.digits, rhs.digits)};
  }
  return {rhs.negative, SubtractUnsignedDecimalDigits(rhs.digits, lhs.digits)};
}

inline SignedDecimalInteger AddSignedDecimalInteger(const SignedDecimalInteger& lhs, int64_t rhs) {
  return AddSignedDecimalInteger(lhs, MakeSignedDecimalInteger(rhs));
}

inline SignedDecimalInteger NegateSignedDecimalInteger(SignedDecimalInteger value) {
  if (value.digits != "0") {
    value.negative = !value.negative;
  }
  return value;
}

inline std::optional<int64_t> TryConvertSignedDecimalIntegerToInt64(
    const SignedDecimalInteger& value
) {
  const uint64_t limit = value.negative
                             ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1
                             : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  uint64_t magnitude = 0;
  for (char digit_char : value.digits) {
    const uint64_t digit = static_cast<uint64_t>(digit_char - '0');
    if (magnitude > (limit - digit) / 10) {
      return std::nullopt;
    }
    magnitude = magnitude * 10 + digit;
  }
  if (!value.negative) {
    return static_cast<int64_t>(magnitude);
  }
  if (magnitude == limit) {
    return std::numeric_limits<int64_t>::min();
  }
  return -static_cast<int64_t>(magnitude);
}

}  // namespace xgrammar

#endif  // XGRAMMAR_SUPPORT_SIGNED_DECIMAL_H_
