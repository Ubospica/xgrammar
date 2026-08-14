/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/json_schema_converter.cc
 * \brief Implementation of JSONSchemaConverter and related utilities.
 */
#include "json_schema_converter.h"

#include <picojson.h>

#include <algorithm>
#include <bitset>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "fsm_builder.h"
#include "grammar_builder.h"
#include "grammar_functor.h"
#include "json_schema_converter_ext.h"
#include "regex_converter.h"
#include "support/logging.h"
#include "support/signed_decimal.h"

namespace xgrammar {

// ==================== Spec ToString implementations ====================

std::string IntegerSpec::ToString() const {
  return "IntegerSpec{minimum=" + (minimum.has_value() ? *minimum : "null") +
         ", maximum=" + (maximum.has_value() ? *maximum : "null") +
         ", exclusive_minimum=" + (exclusive_minimum.has_value() ? *exclusive_minimum : "null") +
         ", exclusive_maximum=" + (exclusive_maximum.has_value() ? *exclusive_maximum : "null") +
         ", multiple_of=" + (multiple_of.has_value() ? std::to_string(*multiple_of) : "null") + "}";
}

std::string NumberSpec::ToString() const {
  std::string multiple_of = "null";
  if (this->multiple_of.has_value()) {
    multiple_of =
        std::to_string(this->multiple_of->first) + "e-" + std::to_string(this->multiple_of->second);
  }
  return "NumberSpec{minimum=" + (minimum.has_value() ? *minimum : "null") +
         ", maximum=" + (maximum.has_value() ? *maximum : "null") +
         ", exclusive_minimum=" + (exclusive_minimum.has_value() ? *exclusive_minimum : "null") +
         ", exclusive_maximum=" + (exclusive_maximum.has_value() ? *exclusive_maximum : "null") +
         ", multiple_of=" + multiple_of + "}";
}

std::string StringSpec::ToString() const {
  return "StringSpec{pattern=" + (pattern.has_value() ? "\"" + *pattern + "\"" : "null") +
         ", format=" + (format.has_value() ? "\"" + *format + "\"" : "null") +
         ", min_length=" + std::to_string(min_length) +
         ", max_length=" + std::to_string(max_length) + "}";
}

std::string BooleanSpec::ToString() const { return "BooleanSpec{}"; }

std::string NullSpec::ToString() const { return "NullSpec{}"; }

std::string AnySpec::ToString() const { return "AnySpec{}"; }

std::string NeverSpec::ToString() const { return "NeverSpec{}"; }

std::string ArraySpec::ToString() const {
  return "ArraySpec{prefix_items.size()=" + std::to_string(prefix_items.size()) +
         ", allow_additional_items=" + (allow_additional_items ? "true" : "false") +
         ", additional_items=" + (additional_items ? "SchemaSpec" : "null") +
         ", min_items=" + std::to_string(min_items) + ", max_items=" + std::to_string(max_items) +
         "}";
}

std::string ObjectSpec::ToString() const {
  std::string s =
      "ObjectSpec{properties.size()=" + std::to_string(properties.size()) + ", properties=[";
  for (size_t i = 0; i < properties.size(); ++i) {
    if (i != 0) s += ", ";
    s += properties[i].name;
  }
  s += "], pattern_properties.size()=" + std::to_string(pattern_properties.size()) + ", required=[";
  bool first = true;
  for (const auto& r : required) {
    if (!first) s += ", ";
    s += r;
    first = false;
  }
  s +=
      std::string("], allow_additional_properties=") +
      (allow_additional_properties ? "true" : "false") +
      ", additional_properties_schema=" + (additional_properties_schema ? "SchemaSpec" : "null") +
      ", allow_unevaluated_properties=" + (allow_unevaluated_properties ? "true" : "false") +
      ", unevaluated_properties_schema=" + (unevaluated_properties_schema ? "SchemaSpec" : "null") +
      ", property_names=" + (property_names ? "SchemaSpec" : "null") +
      ", min_properties=" + std::to_string(min_properties) +
      ", max_properties=" + std::to_string(max_properties) + "}";
  return s;
}

std::string ConstSpec::ToString() const { return "ConstSpec{json_value=\"" + json_value + "\"}"; }

std::string EnumSpec::ToString() const {
  std::string s =
      "EnumSpec{json_values.size()=" + std::to_string(json_values.size()) + ", json_values=[";
  for (size_t i = 0; i < json_values.size(); ++i) {
    if (i != 0) s += ", ";
    s += "\"" + json_values[i] + "\"";
  }
  s += "]}";
  return s;
}

std::string RefSpec::ToString() const { return "RefSpec{uri=\"" + uri + "\"}"; }

std::string AnyOfSpec::ToString() const {
  return "AnyOfSpec{options.size()=" + std::to_string(options.size()) + "}";
}

std::string OneOfSpec::ToString() const {
  return "OneOfSpec{options.size()=" + std::to_string(options.size()) + "}";
}

std::string AllOfSpec::ToString() const {
  return "AllOfSpec{schemas.size()=" + std::to_string(schemas.size()) + "}";
}

std::string TypeArraySpec::ToString() const {
  return "TypeArraySpec{type_schemas.size()=" + std::to_string(type_schemas.size()) + "}";
}

std::string SchemaSpec::ToString() const {
  std::string spec_str;
  std::visit([&spec_str](const auto& s) { spec_str = s.ToString(); }, spec);
  return "SchemaSpec{spec=" + spec_str + ", cache_key=\"" + cache_key + "\", rule_name_hint=\"" +
         rule_name_hint + "\"}";
}

// ==================== SchemaParser (Internal) ====================

namespace {

enum class SchemaErrorType : int {
  kInvalidSchema = 0,
  kUnsatisfiableSchema = 1,
  kUnsupportedSchema = 2,
};

using SchemaError = TypedError<SchemaErrorType>;

// Unbounded integer multipleOf emits a modulo DFA: states ~= N, transitions ~= 10N.
// Fail closed above the cap to keep generated grammars bounded.
constexpr int64_t kIntegerMultipleOfMax = 1024;
constexpr int64_t kIntegerMultipleOfRangeWidthMax = 10000;
// Runtime states store remainders in int32. This cap also bounds per-byte arithmetic.
constexpr int32_t kNumberMultipleOfCoefficientMax = 1000000000;
constexpr int kJSONSchemaPatternDFAStateLimit = 4096;
constexpr char kNegatedJSONSchemaPatternUnionPrefixData[] =
    "xgrammar-internal:negated-json-schema-pattern-union:";
constexpr std::string_view kNegatedJSONSchemaPatternUnionPrefix(
    kNegatedJSONSchemaPatternUnionPrefixData, sizeof(kNegatedJSONSchemaPatternUnionPrefixData) - 1
);
// Tracking the subset of required keys fixes the common any-order object case without making
// grammar size exponential for schemas with hundreds of required fields. Above either bound the
// legacy count-only construction remains the bounded fallback until required-key tracking moves
// into a compact runtime side constraint.
constexpr size_t kAnyOrderRequiredPropertyLimit = 12;
constexpr size_t kAnyOrderRequiredSubsetStateLimit = 4096;
// Matching every permutation of a finite object requires remembering the subset of properties
// already emitted. Keep that exact construction bounded; above this limit conversion fails
// explicitly instead of silently changing any_order=True into fixed-order semantics.
constexpr size_t kFiniteJSONValueAnyOrderPropertyLimit = 12;
constexpr char kExactIntegerBoundPrefixData[] = "\0xgrammar:exact-integer-bound:";
constexpr std::string_view kExactIntegerBoundPrefix(
    kExactIntegerBoundPrefixData, sizeof(kExactIntegerBoundPrefixData) - 1
);
constexpr char kEscapedExactIntegerBoundPrefixData[] = "\0xgrammar:escaped-exact-integer-bound:";
constexpr std::string_view kEscapedExactIntegerBoundPrefix(
    kEscapedExactIntegerBoundPrefixData, sizeof(kEscapedExactIntegerBoundPrefixData) - 1
);
constexpr char kExactNumberBoundPrefixData[] = "\0xgrammar:exact-number-bound:";
constexpr std::string_view kExactNumberBoundPrefix(
    kExactNumberBoundPrefixData, sizeof(kExactNumberBoundPrefixData) - 1
);
constexpr char kEscapedExactNumberBoundPrefixData[] = "\0xgrammar:escaped-exact-number-bound:";
constexpr std::string_view kEscapedExactNumberBoundPrefix(
    kEscapedExactNumberBoundPrefixData, sizeof(kEscapedExactNumberBoundPrefixData) - 1
);
constexpr char kExactNumberMultipleOfPrefixData[] = "\0xgrammar:exact-number-multiple-of:";
constexpr std::string_view kExactNumberMultipleOfPrefix(
    kExactNumberMultipleOfPrefixData, sizeof(kExactNumberMultipleOfPrefixData) - 1
);
constexpr char kEscapedExactNumberMultipleOfPrefixData[] =
    "\0xgrammar:escaped-exact-number-multiple-of:";
constexpr std::string_view kEscapedExactNumberMultipleOfPrefix(
    kEscapedExactNumberMultipleOfPrefixData, sizeof(kEscapedExactNumberMultipleOfPrefixData) - 1
);
constexpr char kExactJSONNumberPrefixData[] = "\0xgrammar:exact-json-number:";
constexpr std::string_view kExactJSONNumberPrefix(
    kExactJSONNumberPrefixData, sizeof(kExactJSONNumberPrefixData) - 1
);
constexpr char kEscapedExactJSONNumberPrefixData[] = "\0xgrammar:escaped-exact-json-number:";
constexpr std::string_view kEscapedExactJSONNumberPrefix(
    kEscapedExactJSONNumberPrefixData, sizeof(kEscapedExactJSONNumberPrefixData) - 1
);

struct ExactDecimalValue {
  bool negative = false;
  std::string digits;
  // Number of base-10 digits before the decimal point in normalized scientific notation.
  SignedDecimalInteger order;
};

std::optional<ExactDecimalValue> ParseExactDecimalLexeme(const std::string& text) {
  size_t position = 0;
  bool negative = false;
  if (position < text.size() && text[position] == '-') {
    negative = true;
    ++position;
  }
  const size_t integer_begin = position;
  while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
    ++position;
  }
  if (position == integer_begin || (text[integer_begin] == '0' && position - integer_begin != 1)) {
    return std::nullopt;
  }
  std::string digits = text.substr(integer_begin, position - integer_begin);
  int64_t fractional_digits = 0;
  if (position < text.size() && text[position] == '.') {
    const size_t fraction_begin = ++position;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
      digits.push_back(text[position++]);
    }
    if (position == fraction_begin) {
      return std::nullopt;
    }
    fractional_digits = static_cast<int64_t>(position - fraction_begin);
  }

  SignedDecimalInteger exponent;
  bool exponent_negative = false;
  if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
    ++position;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
      exponent_negative = text[position++] == '-';
    }
    const size_t exponent_begin = position;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
      ++position;
    }
    if (position == exponent_begin) {
      return std::nullopt;
    }
    exponent = MakeSignedDecimalInteger(
        exponent_negative, std::string_view(text).substr(exponent_begin, position - exponent_begin)
    );
  }
  if (position != text.size()) {
    return std::nullopt;
  }

  const size_t first_nonzero = digits.find_first_not_of('0');
  if (first_nonzero == std::string::npos) {
    return ExactDecimalValue{false, "0", {}};
  }
  digits.erase(0, first_nonzero);
  SignedDecimalInteger order =
      AddSignedDecimalInteger(exponent, static_cast<int64_t>(digits.size()) - fractional_digits);
  while (digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
  }
  return ExactDecimalValue{negative, std::move(digits), order};
}

int CompareExactDecimalValues(const ExactDecimalValue& lhs, const ExactDecimalValue& rhs) {
  const bool lhs_zero = lhs.digits == "0";
  const bool rhs_zero = rhs.digits == "0";
  if (lhs_zero || rhs_zero) {
    if (lhs_zero && rhs_zero) return 0;
    if (lhs_zero) return rhs.negative ? 1 : -1;
    return lhs.negative ? -1 : 1;
  }
  if (lhs.negative != rhs.negative) {
    return lhs.negative ? -1 : 1;
  }
  int magnitude_cmp = 0;
  const int order_compare = CompareSignedDecimalInteger(lhs.order, rhs.order);
  if (order_compare != 0) {
    magnitude_cmp = order_compare;
  } else {
    const size_t length = std::max(lhs.digits.size(), rhs.digits.size());
    for (size_t index = 0; index < length; ++index) {
      const char lhs_digit = index < lhs.digits.size() ? lhs.digits[index] : '0';
      const char rhs_digit = index < rhs.digits.size() ? rhs.digits[index] : '0';
      if (lhs_digit != rhs_digit) {
        magnitude_cmp = lhs_digit < rhs_digit ? -1 : 1;
        break;
      }
    }
  }
  return lhs.negative ? -magnitude_cmp : magnitude_cmp;
}

int CompareExactDecimalLexemes(const std::string& lhs, const std::string& rhs) {
  auto lhs_value = ParseExactDecimalLexeme(lhs);
  auto rhs_value = ParseExactDecimalLexeme(rhs);
  XGRAMMAR_CHECK(lhs_value.has_value() && rhs_value.has_value());
  return CompareExactDecimalValues(*lhs_value, *rhs_value);
}

std::optional<std::pair<int32_t, int32_t>> ParseExactNumberMultipleOfLexeme(const std::string& text
) {
  size_t position = 0;
  if (position < text.size() && text[position] == '+') {
    ++position;
  }
  if (position >= text.size() || text[position] == '-') {
    return std::nullopt;
  }
  size_t integer_begin = position;
  while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
    ++position;
  }
  if (position == integer_begin || (text[integer_begin] == '0' && position - integer_begin != 1)) {
    return std::nullopt;
  }
  std::string digits = text.substr(integer_begin, position - integer_begin);
  int64_t fractional_digits = 0;
  if (position < text.size() && text[position] == '.') {
    size_t fraction_begin = ++position;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
      digits.push_back(text[position++]);
    }
    if (position == fraction_begin) {
      return std::nullopt;
    }
    fractional_digits = static_cast<int64_t>(position - fraction_begin);
  }
  int64_t exponent = 0;
  bool exponent_negative = false;
  if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
    ++position;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
      exponent_negative = text[position++] == '-';
    }
    size_t exponent_begin = position;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
      if (exponent > 1000000) {
        return std::nullopt;
      }
      exponent = exponent * 10 + (text[position++] - '0');
    }
    if (position == exponent_begin) {
      return std::nullopt;
    }
  }
  if (position != text.size()) {
    return std::nullopt;
  }
  if (exponent_negative) {
    exponent = -exponent;
  }
  size_t first_nonzero = digits.find_first_not_of('0');
  if (first_nonzero == std::string::npos) {
    return std::nullopt;
  }
  digits.erase(0, first_nonzero);
  int64_t decimal_scale = fractional_digits - exponent;
  while (digits.size() > 1 && digits.back() == '0') {
    digits.pop_back();
    --decimal_scale;
  }
  int64_t coefficient = 0;
  for (char digit : digits) {
    coefficient = coefficient * 10 + (digit - '0');
    if (coefficient > kNumberMultipleOfCoefficientMax) {
      return std::nullopt;
    }
  }
  if (decimal_scale < std::numeric_limits<int32_t>::min() ||
      decimal_scale > std::numeric_limits<int32_t>::max()) {
    return std::nullopt;
  }
  return std::make_pair(static_cast<int32_t>(coefficient), static_cast<int32_t>(decimal_scale));
}

std::optional<std::string> ParseCanonicalIntegerLexeme(const std::string& text) {
  size_t position = 0;
  bool negative = false;
  if (position < text.size() && text[position] == '-') {
    negative = true;
    ++position;
  }
  size_t integer_begin = position;
  while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
    ++position;
  }
  if (position == integer_begin) {
    return std::nullopt;
  }
  // A JSON integer part is either exactly zero or starts with a non-zero digit.
  if (text[integer_begin] == '0' && position - integer_begin != 1) {
    return std::nullopt;
  }
  std::string digits = text.substr(integer_begin, position - integer_begin);
  int64_t fractional_digits = 0;
  if (position < text.size() && text[position] == '.') {
    size_t fraction_begin = ++position;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
      digits.push_back(text[position++]);
    }
    if (position == fraction_begin) {
      return std::nullopt;
    }
    fractional_digits = static_cast<int64_t>(position - fraction_begin);
  }

  int64_t exponent = 0;
  if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
    ++position;
    bool exponent_negative = false;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
      exponent_negative = text[position++] == '-';
    }
    size_t exponent_begin = position;
    while (position < text.size() && std::isdigit(static_cast<unsigned char>(text[position]))) {
      if (exponent > 100000) {
        return std::nullopt;
      }
      exponent = exponent * 10 + (text[position++] - '0');
    }
    if (position == exponent_begin) {
      return std::nullopt;
    }
    if (exponent_negative) {
      exponent = -exponent;
    }
  }
  if (position != text.size()) {
    return std::nullopt;
  }

  int64_t decimal_shift = exponent - fractional_digits;
  if (decimal_shift < 0) {
    size_t removed_digits = static_cast<size_t>(-decimal_shift);
    if (removed_digits > digits.size()) {
      return std::nullopt;
    }
    if (!std::all_of(digits.end() - removed_digits, digits.end(), [](char digit) {
          return digit == '0';
        })) {
      return std::nullopt;
    }
    digits.resize(digits.size() - removed_digits);
  } else {
    if (decimal_shift > 4096) {
      return std::nullopt;
    }
    digits.append(static_cast<size_t>(decimal_shift), '0');
  }
  if (digits.empty()) {
    digits = "0";
  }
  size_t first_nonzero = digits.find_first_not_of('0');
  digits = first_nonzero == std::string::npos ? "0" : digits.substr(first_nonzero);
  if (negative && digits != "0") {
    digits.insert(digits.begin(), '-');
  }
  return digits;
}

std::optional<std::string> DecodeExactIntegerBound(const picojson::value& value) {
  if (!value.is<std::string>()) {
    return std::nullopt;
  }
  const std::string& text = value.get<std::string>();
  if (text.size() <= kExactIntegerBoundPrefix.size() ||
      text.compare(0, kExactIntegerBoundPrefix.size(), kExactIntegerBoundPrefix) != 0) {
    return std::nullopt;
  }
  return text.substr(kExactIntegerBoundPrefix.size());
}

std::optional<std::string> DecodeExactNumberBound(const picojson::value& value) {
  if (!value.is<std::string>()) {
    return std::nullopt;
  }
  const std::string& text = value.get<std::string>();
  if (text.size() <= kExactNumberBoundPrefix.size() ||
      text.compare(0, kExactNumberBoundPrefix.size(), kExactNumberBoundPrefix) != 0) {
    return std::nullopt;
  }
  return text.substr(kExactNumberBoundPrefix.size());
}

std::optional<std::pair<int32_t, int32_t>> DecodeExactNumberMultipleOf(const picojson::value& value
) {
  if (!value.is<std::string>()) {
    return std::nullopt;
  }
  const std::string& text = value.get<std::string>();
  if (text.size() <= kExactNumberMultipleOfPrefix.size() ||
      text.compare(0, kExactNumberMultipleOfPrefix.size(), kExactNumberMultipleOfPrefix) != 0) {
    return std::nullopt;
  }
  return ParseExactNumberMultipleOfLexeme(text.substr(kExactNumberMultipleOfPrefix.size()));
}

std::optional<std::string> DecodeExactJSONNumber(const picojson::value& value) {
  if (!value.is<std::string>()) {
    return std::nullopt;
  }
  const std::string& text = value.get<std::string>();
  if (text.size() <= kExactJSONNumberPrefix.size() ||
      text.compare(0, kExactJSONNumberPrefix.size(), kExactJSONNumberPrefix) != 0) {
    return std::nullopt;
  }
  std::string lexeme = text.substr(kExactJSONNumberPrefix.size());
  return ParseExactDecimalLexeme(lexeme).has_value() ? std::make_optional(std::move(lexeme))
                                                     : std::nullopt;
}

std::optional<std::string> DecodeEscapedExactJSONNumberString(const picojson::value& value) {
  if (!value.is<std::string>()) {
    return std::nullopt;
  }
  const std::string& text = value.get<std::string>();
  if (text.size() < kEscapedExactJSONNumberPrefix.size() ||
      text.compare(0, kEscapedExactJSONNumberPrefix.size(), kEscapedExactJSONNumberPrefix) != 0) {
    return std::nullopt;
  }
  return text.substr(kEscapedExactJSONNumberPrefix.size());
}

std::optional<int64_t> TryConvertToInt64(const std::string& value);

size_t ScanJSONStringToken(const std::string& json, size_t start) {
  XGRAMMAR_DCHECK(start < json.size() && json[start] == '"');
  bool escaped = false;
  for (size_t position = start + 1; position < json.size(); ++position) {
    if (escaped) {
      escaped = false;
    } else if (json[position] == '\\') {
      escaped = true;
    } else if (json[position] == '"') {
      return position + 1;
    }
  }
  return json.size();
}

size_t ScanJSONNumberToken(const std::string& json, size_t start) {
  size_t position = start;
  if (position < json.size() && json[position] == '-') {
    ++position;
  }
  while (position < json.size() && std::isdigit(static_cast<unsigned char>(json[position]))) {
    ++position;
  }
  if (position < json.size() && json[position] == '.') {
    ++position;
    while (position < json.size() && std::isdigit(static_cast<unsigned char>(json[position]))) {
      ++position;
    }
  }
  if (position < json.size() && (json[position] == 'e' || json[position] == 'E')) {
    ++position;
    if (position < json.size() && (json[position] == '+' || json[position] == '-')) {
      ++position;
    }
    while (position < json.size() && std::isdigit(static_cast<unsigned char>(json[position]))) {
      ++position;
    }
  }
  return position;
}

class ExactNumericConstraintPreserver {
 public:
  explicit ExactNumericConstraintPreserver(const std::string& schema) : schema_(schema) {
    result_.reserve(schema.size());
  }

  std::string Run() {
    RewriteSchemaValue();
    if (position_ < schema_.size()) {
      result_.append(schema_, position_, schema_.size() - position_);
    }
    return std::move(result_);
  }

 private:
  void CopyWhitespace() {
    size_t begin = position_;
    while (position_ < schema_.size() &&
           std::isspace(static_cast<unsigned char>(schema_[position_]))) {
      ++position_;
    }
    result_.append(schema_, begin, position_ - begin);
  }

  void CopyJSONString() {
    size_t end = ScanJSONStringToken(schema_, position_);
    result_.append(schema_, position_, end - position_);
    position_ = end;
  }

  std::string DecodeJSONString(size_t begin, size_t end) const {
    picojson::value value;
    std::string error = picojson::parse(value, schema_.substr(begin, end - begin));
    return error.empty() && value.is<std::string>() ? value.get<std::string>() : std::string();
  }

  // Copy a value without interpreting nested object keys. This is used for instance-valued
  // keywords such as const and enum, where an object member named "maximum" is data, not a
  // numeric Schema constraint.
  void CopyOpaqueValue() {
    CopyWhitespace();
    if (position_ >= schema_.size()) {
      return;
    }
    if (schema_[position_] == '"') {
      CopyJSONString();
      return;
    }
    if (schema_[position_] != '{' && schema_[position_] != '[') {
      size_t begin = position_;
      while (position_ < schema_.size() && schema_[position_] != ',' && schema_[position_] != '}' &&
             schema_[position_] != ']' &&
             !std::isspace(static_cast<unsigned char>(schema_[position_]))) {
        ++position_;
      }
      result_.append(schema_, begin, position_ - begin);
      return;
    }

    const char opening = schema_[position_];
    const char closing = opening == '{' ? '}' : ']';
    int depth = 0;
    size_t begin = position_;
    while (position_ < schema_.size()) {
      if (schema_[position_] == '"') {
        position_ = ScanJSONStringToken(schema_, position_);
        continue;
      }
      if (schema_[position_] == opening) {
        ++depth;
      } else if (schema_[position_] == closing && --depth == 0) {
        ++position_;
        break;
      }
      ++position_;
    }
    result_.append(schema_, begin, position_ - begin);
  }

  // Preserve every number inside an instance-valued const/enum subtree. JSON Schema compares
  // numbers by mathematical value, while picojson stores non-integers as double and can turn 0.4
  // into 0.40000000000000002 during serialization. Internal strings carry the original decimal
  // lexeme through parsing; colliding user strings are escaped so they retain string semantics.
  void RewriteInstanceValue() {
    CopyWhitespace();
    if (position_ >= schema_.size()) {
      return;
    }
    if (schema_[position_] == '"') {
      size_t begin = position_;
      size_t end = ScanJSONStringToken(schema_, position_);
      std::string decoded = DecodeJSONString(begin, end);
      if (decoded.compare(0, kExactJSONNumberPrefix.size(), kExactJSONNumberPrefix) == 0 ||
          decoded.compare(0, kEscapedExactJSONNumberPrefix.size(), kEscapedExactJSONNumberPrefix) ==
              0) {
        result_ +=
            picojson::value(std::string(kEscapedExactJSONNumberPrefix) + decoded).serialize();
      } else {
        result_.append(schema_, begin, end - begin);
      }
      position_ = end;
      return;
    }
    if (schema_[position_] == '{') {
      result_.push_back(schema_[position_++]);
      while (position_ < schema_.size()) {
        CopyWhitespace();
        if (position_ < schema_.size() && schema_[position_] == '}') {
          result_.push_back(schema_[position_++]);
          return;
        }
        if (position_ >= schema_.size() || schema_[position_] != '"') {
          return;
        }
        CopyJSONString();
        CopyWhitespace();
        if (position_ >= schema_.size() || schema_[position_] != ':') {
          return;
        }
        result_.push_back(schema_[position_++]);
        RewriteInstanceValue();
        CopyWhitespace();
        if (position_ < schema_.size() && schema_[position_] == ',') {
          result_.push_back(schema_[position_++]);
          continue;
        }
        if (position_ < schema_.size() && schema_[position_] == '}') {
          result_.push_back(schema_[position_++]);
        }
        return;
      }
      return;
    }
    if (schema_[position_] == '[') {
      result_.push_back(schema_[position_++]);
      while (position_ < schema_.size()) {
        CopyWhitespace();
        if (position_ < schema_.size() && schema_[position_] == ']') {
          result_.push_back(schema_[position_++]);
          return;
        }
        RewriteInstanceValue();
        CopyWhitespace();
        if (position_ < schema_.size() && schema_[position_] == ',') {
          result_.push_back(schema_[position_++]);
          continue;
        }
        if (position_ < schema_.size() && schema_[position_] == ']') {
          result_.push_back(schema_[position_++]);
        }
        return;
      }
      return;
    }
    if (std::isdigit(static_cast<unsigned char>(schema_[position_])) || schema_[position_] == '-') {
      size_t number_end = ScanJSONNumberToken(schema_, position_);
      std::string number_token = schema_.substr(position_, number_end - position_);
      result_ += picojson::value(std::string(kExactJSONNumberPrefix) + number_token).serialize();
      position_ = number_end;
      return;
    }
    CopyOpaqueValue();
  }

  void RewriteNumberBoundValue() {
    CopyWhitespace();
    if (position_ >= schema_.size()) {
      return;
    }
    if (schema_[position_] == '{' || schema_[position_] == '[') {
      // The outer keyword is invalid as a numeric constraint, but the same member may also be an
      // arbitrary local-$ref target. Preserve exact bounds in that nested schema so resolution is
      // independent of the target member's name.
      RewriteSchemaValue();
      return;
    }
    if (schema_[position_] == '"') {
      size_t begin = position_;
      size_t end = ScanJSONStringToken(schema_, position_);
      std::string decoded = DecodeJSONString(begin, end);
      if (decoded.size() > kExactNumberBoundPrefix.size() &&
          decoded.compare(0, kExactNumberBoundPrefix.size(), kExactNumberBoundPrefix) == 0) {
        result_ += picojson::value(
                       std::string(kEscapedExactNumberBoundPrefix) +
                       decoded.substr(kExactNumberBoundPrefix.size())
        )
                       .serialize();
        position_ = end;
        return;
      }
      if (decoded.size() > kExactIntegerBoundPrefix.size() &&
          decoded.compare(0, kExactIntegerBoundPrefix.size(), kExactIntegerBoundPrefix) == 0) {
        // An external string must never be mistaken for the internal exact-number marker. The
        // constraint remains a string, so ParseInteger/ParseNumber will reject it as before.
        result_ += picojson::value(
                       std::string(kEscapedExactIntegerBoundPrefix) +
                       decoded.substr(kExactIntegerBoundPrefix.size())
        )
                       .serialize();
        position_ = end;
        return;
      }
      CopyJSONString();
      return;
    }
    if (!std::isdigit(static_cast<unsigned char>(schema_[position_])) &&
        schema_[position_] != '-') {
      CopyOpaqueValue();
      return;
    }

    size_t number_end = ScanJSONNumberToken(schema_, position_);
    std::string number_token = schema_.substr(position_, number_end - position_);
    if (ParseExactDecimalLexeme(number_token).has_value()) {
      result_ += picojson::value(std::string(kExactNumberBoundPrefix) + number_token).serialize();
    } else {
      result_ += number_token;
    }
    position_ = number_end;
  }

  void RewriteNumberMultipleOfValue() {
    CopyWhitespace();
    if (position_ >= schema_.size()) {
      return;
    }
    if (schema_[position_] == '{' || schema_[position_] == '[') {
      RewriteSchemaValue();
      return;
    }
    if (schema_[position_] == '"') {
      size_t begin = position_;
      size_t end = ScanJSONStringToken(schema_, position_);
      std::string decoded = DecodeJSONString(begin, end);
      if (decoded.size() > kExactNumberMultipleOfPrefix.size() &&
          decoded.compare(0, kExactNumberMultipleOfPrefix.size(), kExactNumberMultipleOfPrefix) ==
              0) {
        result_ += picojson::value(
                       std::string(kEscapedExactNumberMultipleOfPrefix) +
                       decoded.substr(kExactNumberMultipleOfPrefix.size())
        )
                       .serialize();
        position_ = end;
        return;
      }
      CopyJSONString();
      return;
    }
    if (!std::isdigit(static_cast<unsigned char>(schema_[position_])) &&
        schema_[position_] != '-') {
      CopyOpaqueValue();
      return;
    }
    size_t number_end = ScanJSONNumberToken(schema_, position_);
    std::string number_token = schema_.substr(position_, number_end - position_);
    if (ParseExactNumberMultipleOfLexeme(number_token).has_value()) {
      result_ +=
          picojson::value(std::string(kExactNumberMultipleOfPrefix) + number_token).serialize();
    } else {
      result_ += number_token;
    }
    position_ = number_end;
  }

  void RewriteSchemaArray() {
    CopyWhitespace();
    if (position_ >= schema_.size() || schema_[position_] != '[') {
      CopyOpaqueValue();
      return;
    }
    result_.push_back(schema_[position_++]);
    while (position_ < schema_.size()) {
      CopyWhitespace();
      if (position_ < schema_.size() && schema_[position_] == ']') {
        result_.push_back(schema_[position_++]);
        return;
      }
      RewriteSchemaValue();
      CopyWhitespace();
      if (position_ < schema_.size() && schema_[position_] == ',') {
        result_.push_back(schema_[position_++]);
        continue;
      }
      if (position_ < schema_.size() && schema_[position_] == ']') {
        result_.push_back(schema_[position_++]);
      }
      return;
    }
  }

  void RewriteSchemaMap() {
    CopyWhitespace();
    if (position_ >= schema_.size() || schema_[position_] != '{') {
      CopyOpaqueValue();
      return;
    }
    result_.push_back(schema_[position_++]);
    while (position_ < schema_.size()) {
      CopyWhitespace();
      if (position_ < schema_.size() && schema_[position_] == '}') {
        result_.push_back(schema_[position_++]);
        return;
      }
      if (position_ >= schema_.size() || schema_[position_] != '"') {
        CopyOpaqueValue();
        return;
      }
      CopyJSONString();
      CopyWhitespace();
      if (position_ >= schema_.size() || schema_[position_] != ':') {
        return;
      }
      result_.push_back(schema_[position_++]);
      RewriteSchemaValue();
      CopyWhitespace();
      if (position_ < schema_.size() && schema_[position_] == ',') {
        result_.push_back(schema_[position_++]);
        continue;
      }
      if (position_ < schema_.size() && schema_[position_] == '}') {
        result_.push_back(schema_[position_++]);
      }
      return;
    }
  }

  void RewriteSchemaObject() {
    result_.push_back(schema_[position_++]);
    while (position_ < schema_.size()) {
      CopyWhitespace();
      if (position_ < schema_.size() && schema_[position_] == '}') {
        result_.push_back(schema_[position_++]);
        return;
      }
      if (position_ >= schema_.size() || schema_[position_] != '"') {
        CopyOpaqueValue();
        return;
      }
      size_t key_begin = position_;
      size_t key_end = ScanJSONStringToken(schema_, position_);
      std::string key = DecodeJSONString(key_begin, key_end);
      result_.append(schema_, key_begin, key_end - key_begin);
      position_ = key_end;
      CopyWhitespace();
      if (position_ >= schema_.size() || schema_[position_] != ':') {
        return;
      }
      result_.push_back(schema_[position_++]);

      if (key == "minimum" || key == "maximum" || key == "exclusiveMinimum" ||
          key == "exclusiveMaximum") {
        RewriteNumberBoundValue();
      } else if (key == "multipleOf") {
        RewriteNumberMultipleOfValue();
      } else if (key == "properties" || key == "patternProperties" || key == "$defs" ||
                 key == "definitions" || key == "dependentSchemas") {
        RewriteSchemaMap();
      } else if (key == "prefixItems" || key == "allOf" || key == "anyOf" || key == "oneOf") {
        RewriteSchemaArray();
      } else if (key == "const" || key == "enum") {
        RewriteInstanceValue();
      } else if (key == "default" || key == "examples" || key == "example") {
        CopyOpaqueValue();
      } else {
        // Recurse through unknown locations as well: a local $ref may legally target an arbitrary
        // object outside $defs. Instance-valued keywords above are the only opaque subtrees.
        RewriteSchemaValue();
      }

      CopyWhitespace();
      if (position_ < schema_.size() && schema_[position_] == ',') {
        result_.push_back(schema_[position_++]);
        continue;
      }
      if (position_ < schema_.size() && schema_[position_] == '}') {
        result_.push_back(schema_[position_++]);
      }
      return;
    }
  }

  void RewriteSchemaValue() {
    CopyWhitespace();
    if (position_ >= schema_.size()) {
      return;
    }
    if (schema_[position_] == '{') {
      RewriteSchemaObject();
    } else if (schema_[position_] == '[') {
      RewriteSchemaArray();
    } else {
      CopyOpaqueValue();
    }
  }

  const std::string& schema_;
  std::string result_;
  size_t position_ = 0;
};

/*! Preserve exact numeric constraint lexemes through picojson.
 *
 * Picojson stores non-integer JSON numbers as double, which loses decimal precision. This
 * schema-aware lexical pass replaces numeric minimum/maximum keyword values with an internal
 * marker. Instance-valued const/enum/default/examples subtrees remain byte-for-byte untouched.
 */
std::string PreserveExactNumericConstraints(const std::string& schema) {
  return ExactNumericConstraintPreserver(schema).Run();
}

struct RegexScanState {
  int group_depth = 0;
  bool in_character_class = false;
  bool escaped = false;
};

void AdvanceRegexScanState(char character, RegexScanState* state) {
  if (state->escaped) {
    state->escaped = false;
    return;
  }
  if (character == '\\') {
    state->escaped = true;
    return;
  }
  if (state->in_character_class) {
    if (character == ']') {
      state->in_character_class = false;
    }
    return;
  }
  if (character == '[') {
    state->in_character_class = true;
  } else if (character == '(') {
    ++state->group_depth;
  } else if (character == ')') {
    --state->group_depth;
  }
}

/*! \brief Return the contents of a transparent group enclosing the complete pattern, if any. */
std::optional<std::string> UnwrapWholeRegexGroup(const std::string& pattern) {
  if (pattern.size() < 2 || pattern.front() != '(') {
    return std::nullopt;
  }
  size_t content_begin = 1;
  if (pattern.size() >= 4 && pattern.compare(0, 3, "(?:") == 0) {
    content_begin = 3;
  } else if (pattern.size() >= 3 && pattern[1] == '?') {
    // Lookarounds, inline flags, and other special groups are not transparent.
    return std::nullopt;
  }

  RegexScanState state;
  for (size_t i = 0; i < pattern.size(); ++i) {
    AdvanceRegexScanState(pattern[i], &state);
    if (state.group_depth == 0 && !state.in_character_class && !state.escaped) {
      if (i != pattern.size() - 1) {
        return std::nullopt;
      }
      return pattern.substr(content_begin, pattern.size() - content_begin - 1);
    }
  }
  return std::nullopt;
}

std::vector<std::string> SplitTopLevelRegexAlternatives(const std::string& pattern) {
  std::vector<std::string> alternatives;
  RegexScanState state;
  size_t alternative_begin = 0;
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (pattern[i] == '|' && state.group_depth == 0 && !state.in_character_class &&
        !state.escaped) {
      alternatives.push_back(pattern.substr(alternative_begin, i - alternative_begin));
      alternative_begin = i + 1;
      continue;
    }
    AdvanceRegexScanState(pattern[i], &state);
  }
  if (!alternatives.empty()) {
    alternatives.push_back(pattern.substr(alternative_begin));
  }
  return alternatives;
}

bool IsRegexSyntaxCharacterAt(const std::string& pattern, size_t target) {
  RegexScanState state;
  for (size_t i = 0; i <= target; ++i) {
    if (i == target) {
      return !state.escaped && !state.in_character_class;
    }
    AdvanceRegexScanState(pattern[i], &state);
  }
  return false;
}

struct JSONSchemaPatternAlternative {
  std::string body;
  bool anchored_at_start;
  bool anchored_at_end;
};

/*! \brief Flatten transparent top-level groups and alternatives, retaining each branch's anchors.
 *
 * Capture values are irrelevant to JSON Schema validation, so transparent groups enclosing a
 * complete alternative can be flattened safely. Grouping the resulting branches by their anchor
 * pair lets all branches with the same search behavior share one wildcard prefix and suffix.
 */
void CollectJSONSchemaPatternAlternatives(
    const std::string& pattern, std::vector<JSONSchemaPatternAlternative>* result
) {
  if (auto unwrapped = UnwrapWholeRegexGroup(pattern)) {
    CollectJSONSchemaPatternAlternatives(*unwrapped, result);
    return;
  }

  auto alternatives = SplitTopLevelRegexAlternatives(pattern);
  if (!alternatives.empty()) {
    for (const auto& alternative : alternatives) {
      CollectJSONSchemaPatternAlternatives(alternative, result);
    }
    return;
  }

  size_t content_begin = 0;
  size_t content_end = pattern.size();
  bool anchored_at_start = false;
  bool anchored_at_end = false;
  while (content_begin < content_end && pattern[content_begin] == '^') {
    anchored_at_start = true;
    ++content_begin;
  }
  while (content_begin < content_end && pattern[content_end - 1] == '$' &&
         IsRegexSyntaxCharacterAt(pattern, content_end - 1)) {
    anchored_at_end = true;
    --content_end;
  }
  result->push_back(
      {pattern.substr(content_begin, content_end - content_begin),
       anchored_at_start,
       anchored_at_end}
  );
}

/*! \brief Rewrite an ECMAScript search pattern for the whole-string grammar regex engine.
 *
 * JSON Schema patterns use search semantics, while grammar regex nodes consume their complete
 * input. Top-level alternatives therefore receive independent wildcard prefixes and suffixes.
 * Transparent outer groups are recursively unwrapped so common generated patterns such as
 * `(^a$)|(^b$)` preserve their branch-local anchors.
 */
std::string RewriteJSONSchemaPatternForFullMatchInternal(const std::string& pattern) {
  std::vector<JSONSchemaPatternAlternative> alternatives;
  CollectJSONSchemaPatternAlternatives(pattern, &alternatives);

  std::array<std::vector<std::string>, 4> grouped_bodies;
  for (auto& alternative : alternatives) {
    const int group =
        (alternative.anchored_at_start ? 2 : 0) | (alternative.anchored_at_end ? 1 : 0);
    grouped_bodies[group].push_back(std::move(alternative.body));
  }

  std::vector<std::string> rewritten_groups;
  for (int group = 0; group < 4; ++group) {
    if (grouped_bodies[group].empty()) {
      continue;
    }
    std::string rewritten;
    if ((group & 2) == 0) {
      rewritten += "(?:[\\s\\S]*)";
    }
    // RegexFSMBuilder accepts an empty regex by itself, but an empty non-capturing group used as
    // one arm of an alternation (for example the rewritten form of `^$|^a$`) is ambiguous to its
    // bracket parser.  `x{0}` is an equivalent epsilon expression that remains well formed inside
    // an alternation.  Keeping the empty branch in the DFA path is also important for JSON-string
    // safety: falling back to decoded CFG character classes would let classes such as `\\S`
    // consume an unescaped closing quote as source text.
    const auto append_body = [&](const std::string& body) {
      rewritten += body.empty() ? "(?:x{0})" : "(?:" + body + ")";
    };
    if (grouped_bodies[group].size() == 1) {
      append_body(grouped_bodies[group][0]);
    } else {
      rewritten += "(?:";
      for (size_t index = 0; index < grouped_bodies[group].size(); ++index) {
        if (index != 0) {
          rewritten.push_back('|');
        }
        append_body(grouped_bodies[group][index]);
      }
      rewritten.push_back(')');
    }
    if ((group & 1) == 0) {
      rewritten += "(?:[\\s\\S]*)";
    }
    rewritten_groups.push_back(std::move(rewritten));
  }

  XGRAMMAR_DCHECK(!rewritten_groups.empty());
  if (rewritten_groups.size() == 1) {
    return std::move(rewritten_groups[0]);
  }
  std::string result = "(?:";
  for (size_t index = 0; index < rewritten_groups.size(); ++index) {
    if (index != 0) {
      result.push_back('|');
    }
    result += "(?:" + rewritten_groups[index] + ")";
  }
  result.push_back(')');
  return result;
}

/*! \brief Build a total DFA for an unanchored union of plain ASCII literals.
 *
 * Regex-to-DFA subset construction is unnecessarily expensive for generated patterns containing
 * hundreds of literal alternatives.  For that exact shape, an Aho--Corasick prefix automaton
 * recognizes whether any literal has occurred in linear construction time.  The accepting state
 * is absorbing because JSON Schema pattern matching uses search semantics: once a substring has
 * matched, the rest of the string cannot invalidate it.
 */
std::optional<FSMWithStartEnd> BuildASCIILiteralSearchFSM(const std::string& pattern) {
  std::vector<JSONSchemaPatternAlternative> alternatives;
  CollectJSONSchemaPatternAlternatives(pattern, &alternatives);
  if (alternatives.empty()) {
    return std::nullopt;
  }

  auto parse_literal = [](const std::string& body) -> std::optional<std::string> {
    static constexpr std::string_view kRegexSyntax = R"(.^$*+?()[]{}|\)";
    static constexpr std::string_view kSafelyEscapedLiteral = R"(.^$*+?()[]{}|\/\-)";
    std::string result;
    result.reserve(body.size());
    for (size_t i = 0; i < body.size(); ++i) {
      const unsigned char byte = static_cast<unsigned char>(body[i]);
      if (byte >= 0x80) {
        return std::nullopt;
      }
      if (body[i] == '\\') {
        if (++i == body.size() || kSafelyEscapedLiteral.find(body[i]) == std::string_view::npos) {
          return std::nullopt;
        }
        result.push_back(body[i]);
      } else {
        if (kRegexSyntax.find(body[i]) != std::string_view::npos) {
          return std::nullopt;
        }
        result.push_back(body[i]);
      }
    }
    return result.empty() ? std::nullopt : std::optional<std::string>(std::move(result));
  };

  std::vector<std::string> literals;
  literals.reserve(alternatives.size());
  for (const auto& alternative : alternatives) {
    if (alternative.anchored_at_start || alternative.anchored_at_end) {
      return std::nullopt;
    }
    auto literal = parse_literal(alternative.body);
    if (!literal.has_value()) {
      return std::nullopt;
    }
    literals.push_back(std::move(*literal));
  }

  struct TrieNode {
    std::array<int32_t, 256> transitions;
    int32_t failure = 0;
    bool terminal = false;

    TrieNode() { transitions.fill(-1); }
  };
  std::vector<TrieNode> nodes(1);
  for (const std::string& literal : literals) {
    int32_t state = 0;
    for (uint8_t byte : literal) {
      int32_t next = nodes[state].transitions[byte];
      if (next < 0) {
        next = static_cast<int32_t>(nodes.size());
        nodes[state].transitions[byte] = next;
        nodes.emplace_back();
      }
      state = next;
    }
    nodes[state].terminal = true;
  }

  std::queue<int32_t> pending;
  for (int32_t byte = 0; byte < 256; ++byte) {
    int32_t& next = nodes[0].transitions[byte];
    if (next < 0) {
      next = 0;
    } else {
      nodes[next].failure = 0;
      pending.push(next);
    }
  }
  while (!pending.empty()) {
    const int32_t state = pending.front();
    pending.pop();
    nodes[state].terminal = nodes[state].terminal || nodes[nodes[state].failure].terminal;
    for (int32_t byte = 0; byte < 256; ++byte) {
      int32_t& next = nodes[state].transitions[byte];
      if (next < 0) {
        next = nodes[nodes[state].failure].transitions[byte];
      } else {
        nodes[next].failure = nodes[nodes[state].failure].transitions[byte];
        pending.push(next);
      }
    }
  }

  const int32_t accepting_state = static_cast<int32_t>(nodes.size());
  FSM fsm(accepting_state + 1);
  for (int32_t state = 0; state < static_cast<int32_t>(nodes.size()); ++state) {
    int32_t range_begin = 0;
    auto target_for = [&](int32_t byte) {
      const int32_t target = nodes[state].transitions[byte];
      return nodes[target].terminal ? accepting_state : target;
    };
    int32_t target = target_for(0);
    for (int32_t byte = 1; byte <= 256; ++byte) {
      const int32_t next_target = byte == 256 ? -1 : target_for(byte);
      if (next_target != target) {
        fsm.AddEdge(state, target, range_begin, byte - 1);
        range_begin = byte;
        target = next_target;
      }
    }
  }
  fsm.AddEdge(accepting_state, accepting_state, 0, 255);
  return FSMWithStartEnd(std::move(fsm), 0, {accepting_state}, true);
}

bool IsNegatedJSONSchemaPatternUnion(const std::string& pattern) {
  return pattern.size() >= kNegatedJSONSchemaPatternUnionPrefix.size() &&
         pattern.compare(
             0, kNegatedJSONSchemaPatternUnionPrefix.size(), kNegatedJSONSchemaPatternUnionPrefix
         ) == 0;
}

Result<std::vector<std::string>> DecodeNegatedJSONSchemaPatternUnion(const std::string& pattern) {
  picojson::value encoded_patterns;
  std::string error = picojson::parse(
      encoded_patterns, pattern.substr(kNegatedJSONSchemaPatternUnionPrefix.size())
  );
  if (!error.empty() || !encoded_patterns.is<picojson::array>()) {
    return ResultErr("Invalid internal negated JSON Schema pattern union");
  }
  std::vector<std::string> result;
  for (const auto& encoded_pattern : encoded_patterns.get<picojson::array>()) {
    if (!encoded_pattern.is<std::string>()) {
      return ResultErr("Invalid member in internal negated JSON Schema pattern union");
    }
    result.push_back(encoded_pattern.get<std::string>());
  }
  if (result.empty()) {
    return ResultErr("Internal negated JSON Schema pattern union must not be empty");
  }
  return ResultOk(std::move(result));
}

std::string EncodeNegatedJSONSchemaPatternUnion(const std::vector<std::string>& patterns) {
  XGRAMMAR_DCHECK(!patterns.empty());
  picojson::array encoded_patterns;
  encoded_patterns.reserve(patterns.size());
  for (const auto& pattern : patterns) encoded_patterns.emplace_back(pattern);
  return std::string(kNegatedJSONSchemaPatternUnionPrefix) +
         picojson::value(std::move(encoded_patterns)).serialize();
}

Result<FSMWithStartEnd> BuildOrdinaryJSONSchemaPatternFSM(
    const std::string& pattern, int max_num_states
) {
  if (auto literal_search = BuildASCIILiteralSearchFSM(pattern)) {
    return ResultOk(std::move(*literal_search));
  }
  auto built = RegexFSMBuilder::Build(RewriteJSONSchemaPatternForFullMatchInternal(pattern));
  if (built.IsErr()) {
    return built;
  }
  auto fsm = std::move(built).Unwrap();
  if (fsm.IsDFA()) {
    return ResultOk(std::move(fsm));
  }
  return fsm.ToDFA(max_num_states);
}

Result<FSMWithStartEnd> BuildJSONSchemaPatternFSMInternal(
    const std::string& pattern, int max_num_states
) {
  if (!IsNegatedJSONSchemaPatternUnion(pattern)) {
    return BuildOrdinaryJSONSchemaPatternFSM(pattern, max_num_states);
  }

  auto decoded = DecodeNegatedJSONSchemaPatternUnion(pattern);
  if (decoded.IsErr()) return ResultErr(std::move(decoded).UnwrapErr());
  std::vector<FSMWithStartEnd> pattern_fsms;
  for (const auto& member : std::move(decoded).Unwrap()) {
    auto built = BuildOrdinaryJSONSchemaPatternFSM(member, max_num_states);
    if (built.IsErr()) return ResultErr(std::move(built).UnwrapErr());
    pattern_fsms.push_back(std::move(built).Unwrap());
  }
  FSMWithStartEnd pattern_union = pattern_fsms.size() == 1 ? std::move(pattern_fsms.front())
                                                           : FSMWithStartEnd::Union(pattern_fsms);
  return pattern_union.Not(max_num_states);
}

struct SimpleCharacterClassRepeat {
  std::bitset<256> allowed_bytes;
  int min_count;
  int max_count;
};

bool ParseNonnegativeDecimal(const std::string& text, int* value) {
  if (text.empty()) {
    return false;
  }
  int result = 0;
  for (char character : text) {
    if (character < '0' || character > '9') {
      return false;
    }
    int digit = character - '0';
    if (result > (std::numeric_limits<int>::max() - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

/*! \brief Recognize an anchored character class followed by one repeat quantifier.
 *
 * This deliberately stays narrow. The general regex path remains the semantic fallback, while
 * common identifier patterns such as `^[A-Za-z0-9_-]*$` and
 * `^[A-Za-z0-9_-]{1,255}$` avoid expanding every repetition before JSON escape spellings are
 * introduced.
 */
std::optional<SimpleCharacterClassRepeat> ParseSimpleCharacterClassRepeat(const std::string& pattern
) {
  if (pattern.size() < 6 || pattern[0] != '^' || pattern[1] != '[' || pattern.back() != '$') {
    return std::nullopt;
  }

  bool escaped = false;
  size_t class_end = std::string::npos;
  for (size_t index = 2; index + 1 < pattern.size(); ++index) {
    if (escaped) {
      escaped = false;
    } else if (pattern[index] == '\\') {
      escaped = true;
    } else if (pattern[index] == ']') {
      class_end = index;
      break;
    }
  }
  if (class_end == std::string::npos) {
    return std::nullopt;
  }

  int min_count = 0;
  int max_count = -1;
  const std::string quantifier = pattern.substr(class_end + 1, pattern.size() - class_end - 2);
  if (quantifier == "*") {
    min_count = 0;
  } else if (quantifier == "+") {
    min_count = 1;
  } else if (quantifier.size() >= 3 && quantifier.front() == '{' && quantifier.back() == '}') {
    const std::string bounds = quantifier.substr(1, quantifier.size() - 2);
    const size_t comma = bounds.find(',');
    if (comma == std::string::npos) {
      if (!ParseNonnegativeDecimal(bounds, &min_count)) {
        return std::nullopt;
      }
      max_count = min_count;
    } else if (bounds.find(',', comma + 1) != std::string::npos ||
               !ParseNonnegativeDecimal(bounds.substr(0, comma), &min_count) ||
               (!bounds.substr(comma + 1).empty() &&
                !ParseNonnegativeDecimal(bounds.substr(comma + 1), &max_count))) {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }
  if (max_count != -1 && min_count > max_count) {
    return std::nullopt;
  }

  auto class_result = RegexFSMBuilder::Build(pattern.substr(1, class_end));
  if (class_result.IsErr()) {
    return std::nullopt;
  }
  auto class_fsm = std::move(class_result).Unwrap();
  if (class_fsm.GetEnds().size() != 1) {
    return std::nullopt;
  }
  const int start = class_fsm.GetStart();
  const int end = class_fsm.GetEnds()[0];
  std::bitset<256> allowed_bytes;
  for (int state = 0; state < class_fsm.NumStates(); ++state) {
    for (const auto& edge : class_fsm.GetFsm().GetEdges(state)) {
      if (state != start || edge.target != end || !edge.IsCharRange()) {
        return std::nullopt;
      }
      for (int byte = edge.min; byte <= edge.max; ++byte) {
        allowed_bytes.set(byte);
      }
    }
  }
  if (allowed_bytes.none()) {
    return std::nullopt;
  }
  for (int byte = 0x80; byte < 256; ++byte) {
    if (allowed_bytes.test(byte)) {
      // Grammar character classes use Unicode code points rather than raw UTF-8 bytes. Keep the
      // byte-oriented general FSM path as the authority for non-ASCII classes.
      return std::nullopt;
    }
  }
  return SimpleCharacterClassRepeat{allowed_bytes, min_count, max_count};
}

/*! \brief Tighten an unanchored character-class search when the whole string has the same exact
 * length as the search match's lower bound.
 *
 * For example, a ten-character string contains `[0-9]{10}` if and only if all ten characters are
 * digits. Adding whole-string anchors is therefore exact and lets the compact decoded-character
 * repeat path handle a common pattern + length combination. The transformation is deliberately
 * limited to patterns that the anchored simple-repeat parser accepts and whose minimum match
 * length equals the known whole-string length.
 */
std::optional<std::string> AnchorExactLengthCharacterClassSearch(
    const std::string& pattern, int32_t exact_length
) {
  if (pattern.empty() || pattern.front() == '^' || pattern.back() == '$') {
    return std::nullopt;
  }
  std::string anchored_pattern = "^" + pattern + "$";
  auto simple_repeat = ParseSimpleCharacterClassRepeat(anchored_pattern);
  if (!simple_repeat.has_value() || simple_repeat->min_count != exact_length) {
    return std::nullopt;
  }
  return anchored_pattern;
}

bool IsMultipleOf(int64_t value, int64_t multiple_of) { return (value % multiple_of) == 0; }

bool HasMultipleInRange(int64_t start, int64_t end, int64_t multiple_of) {
  for (int64_t value = start; value <= end; ++value) {
    if (IsMultipleOf(value, multiple_of)) return true;
    if (value == std::numeric_limits<int64_t>::max()) break;
  }
  return false;
}

constexpr const char* kUnsupportedOneOfMessage =
    "oneOf with overlapping or non-provably-disjoint branches cannot be represented exactly; "
    "falling back to anyOf semantics";

bool IsSchemaAnnotationKey(const std::string& key) {
  static const std::unordered_set<std::string> kAnnotationKeys = {
      "$comment",
      "$defs",
      "$id",
      "$schema",
      "default",
      "definitions",
      "deprecated",
      "description",
      "example",
      "examples",
      "id",
      "readOnly",
      "title",
      "writeOnly",
  };
  return kAnnotationKeys.count(key) != 0;
}

bool IsKnownSchemaAssertionKeyword(const std::string& keyword) {
  static const std::unordered_set<std::string> kAssertionKeywords = {
      "$ref",
      "additionalItems",
      "additionalProperties",
      "allOf",
      "anyOf",
      "const",
      "contains",
      "contentEncoding",
      "contentMediaType",
      "dependencies",
      "dependentRequired",
      "dependentSchemas",
      "else",
      "enum",
      "exclusiveMaximum",
      "exclusiveMinimum",
      "format",
      "if",
      "items",
      "maxContains",
      "maxItems",
      "maxLength",
      "maxProperties",
      "maximum",
      "minContains",
      "minItems",
      "minLength",
      "minProperties",
      "minimum",
      "multipleOf",
      "not",
      "oneOf",
      "pattern",
      "patternProperties",
      "prefixItems",
      "properties",
      "propertyNames",
      "required",
      "then",
      "type",
      "unevaluatedItems",
      "unevaluatedProperties",
      "uniqueItems",
  };
  return kAssertionKeywords.count(keyword) != 0;
}

bool HasOnlyKeys(
    const picojson::object& schema, const std::unordered_set<std::string>& allowed_keys
) {
  for (const auto& [key, _] : schema) {
    if (allowed_keys.count(key) == 0 && !IsSchemaAnnotationKey(key)) {
      return false;
    }
  }
  return true;
}

bool IsSupportedJSONType(const std::string& type) {
  static const std::unordered_set<std::string> kTypes = {
      "null",
      "boolean",
      "object",
      "array",
      "number",
      "string",
      "integer",
  };
  return kTypes.count(type) != 0;
}

bool NormalizeTypeSet(
    const picojson::value& type_value, std::unordered_set<std::string>* type_set
) {
  if (type_value.is<std::string>()) {
    const auto& type = type_value.get<std::string>();
    if (!IsSupportedJSONType(type)) {
      return false;
    }
    type_set->insert(type);
    return true;
  }
  if (!type_value.is<picojson::array>()) {
    return false;
  }

  const auto& type_array = type_value.get<picojson::array>();
  if (type_array.empty()) {
    return false;
  }
  for (const auto& item : type_array) {
    if (!item.is<std::string>()) {
      return false;
    }
    const auto& type = item.get<std::string>();
    if (!IsSupportedJSONType(type)) {
      return false;
    }
    type_set->insert(type);
  }
  return true;
}

bool IsNumericValue(const picojson::value& value) {
  return value.is<int64_t>() || value.is<double>() || DecodeExactJSONNumber(value).has_value();
}

bool IsIntegerValue(const picojson::value& value) {
  if (auto exact = DecodeExactJSONNumber(value)) {
    return ParseCanonicalIntegerLexeme(*exact).has_value();
  }
  if (value.is<int64_t>()) {
    return true;
  }
  if (!value.is<double>()) {
    return false;
  }
  double number = value.get<double>();
  return std::isfinite(number) && std::floor(number) == number;
}

bool JSONValuesMayOverlap(const picojson::value& lhs, const picojson::value& rhs) {
  if (IsNumericValue(lhs) || IsNumericValue(rhs)) {
    if (!IsNumericValue(lhs) || !IsNumericValue(rhs)) {
      return false;
    }
    auto lexeme = [](const picojson::value& value) {
      if (auto exact = DecodeExactJSONNumber(value)) return *exact;
      return value.serialize(false);
    };
    return CompareExactDecimalLexemes(lexeme(lhs), lexeme(rhs)) == 0;
  }
  if (lhs.is<picojson::null>() || rhs.is<picojson::null>()) {
    return lhs.is<picojson::null>() && rhs.is<picojson::null>();
  }
  if (lhs.is<bool>() || rhs.is<bool>()) {
    return lhs.is<bool>() && rhs.is<bool>() && lhs.get<bool>() == rhs.get<bool>();
  }
  if (lhs.is<std::string>() || rhs.is<std::string>()) {
    return lhs.is<std::string>() && rhs.is<std::string>() &&
           lhs.get<std::string>() == rhs.get<std::string>();
  }
  if (lhs.is<picojson::array>() || rhs.is<picojson::array>()) {
    if (!lhs.is<picojson::array>() || !rhs.is<picojson::array>()) {
      return false;
    }
    const auto& lhs_array = lhs.get<picojson::array>();
    const auto& rhs_array = rhs.get<picojson::array>();
    if (lhs_array.size() != rhs_array.size()) {
      return false;
    }
    for (size_t i = 0; i < lhs_array.size(); ++i) {
      if (!JSONValuesMayOverlap(lhs_array[i], rhs_array[i])) {
        return false;
      }
    }
    return true;
  }
  if (lhs.is<picojson::object>() || rhs.is<picojson::object>()) {
    if (!lhs.is<picojson::object>() || !rhs.is<picojson::object>()) {
      return false;
    }
    const auto& lhs_object = lhs.get<picojson::object>();
    const auto& rhs_object = rhs.get<picojson::object>();
    if (lhs_object.size() != rhs_object.size()) {
      return false;
    }
    for (const auto& [key, lhs_value] : lhs_object) {
      auto rhs_it = rhs_object.find(key);
      if (rhs_it == rhs_object.end() || !JSONValuesMayOverlap(lhs_value, rhs_it->second)) {
        return false;
      }
    }
    return true;
  }
  return lhs.serialize() == rhs.serialize();
}

bool ValueMatchesType(const picojson::value& value, const std::string& type) {
  if (type == "null") {
    return value.is<picojson::null>();
  }
  if (type == "boolean") {
    return value.is<bool>();
  }
  if (type == "string") {
    return value.is<std::string>() && !DecodeExactJSONNumber(value).has_value();
  }
  if (type == "integer") {
    return IsIntegerValue(value);
  }
  if (type == "number") {
    return IsNumericValue(value);
  }
  if (type == "array") {
    return value.is<picojson::array>();
  }
  if (type == "object") {
    return value.is<picojson::object>();
  }
  return false;
}

bool IsRangeWidthOverCap(int64_t start, int64_t end, int64_t cap) {
  uint64_t cap_u = static_cast<uint64_t>(cap);
  if (start <= 0 && end >= 0) {
    // Count [start, end] inclusively without evaluating -INT64_MIN or overflowing the sum.
    uint64_t negative_count = start < 0 ? static_cast<uint64_t>(-(start + 1)) + 1 : 0;
    if (negative_count > cap_u) return true;
    uint64_t remaining = cap_u - negative_count;
    if (remaining == 0) return true;
    --remaining;  // zero
    uint64_t positive_count = end > 0 ? static_cast<uint64_t>(end) : 0;
    return positive_count > remaining;
  }

  uint64_t value_count = static_cast<uint64_t>(end - start) + 1;
  return value_count > cap_u;
}

std::string IntegerMagnitude(const std::string& value) {
  return !value.empty() && value.front() == '-' ? value.substr(1) : value;
}

int CompareCanonicalIntegers(const std::string& lhs, const std::string& rhs) {
  const bool lhs_negative = !lhs.empty() && lhs.front() == '-';
  const bool rhs_negative = !rhs.empty() && rhs.front() == '-';
  if (lhs_negative != rhs_negative) {
    return lhs_negative ? -1 : 1;
  }
  const std::string lhs_magnitude = IntegerMagnitude(lhs);
  const std::string rhs_magnitude = IntegerMagnitude(rhs);
  int magnitude_comparison = 0;
  if (lhs_magnitude.size() != rhs_magnitude.size()) {
    magnitude_comparison = lhs_magnitude.size() < rhs_magnitude.size() ? -1 : 1;
  } else if (lhs_magnitude != rhs_magnitude) {
    magnitude_comparison = lhs_magnitude < rhs_magnitude ? -1 : 1;
  }
  return lhs_negative ? -magnitude_comparison : magnitude_comparison;
}

std::string IncrementMagnitude(std::string magnitude) {
  for (size_t i = magnitude.size(); i-- > 0;) {
    if (magnitude[i] != '9') {
      ++magnitude[i];
      return magnitude;
    }
    magnitude[i] = '0';
  }
  return "1" + magnitude;
}

std::string DecrementMagnitude(std::string magnitude) {
  XGRAMMAR_DCHECK(magnitude != "0");
  for (size_t i = magnitude.size(); i-- > 0;) {
    if (magnitude[i] != '0') {
      --magnitude[i];
      break;
    }
    magnitude[i] = '9';
  }
  size_t first_nonzero = magnitude.find_first_not_of('0');
  return first_nonzero == std::string::npos ? "0" : magnitude.substr(first_nonzero);
}

std::string IncrementInteger(const std::string& value) {
  if (!value.empty() && value.front() == '-') {
    std::string magnitude = DecrementMagnitude(value.substr(1));
    return magnitude == "0" ? "0" : "-" + magnitude;
  }
  return IncrementMagnitude(value);
}

std::string DecrementInteger(const std::string& value) {
  if (!value.empty() && value.front() == '-') {
    return "-" + IncrementMagnitude(value.substr(1));
  }
  if (value == "0") {
    return "-1";
  }
  return DecrementMagnitude(value);
}

std::optional<int64_t> TryConvertToInt64(const std::string& value) {
  int64_t result = 0;
  const char* begin = value.data();
  const char* end = begin + value.size();
  auto parsed = std::from_chars(begin, end, result);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return std::nullopt;
  }
  return result;
}

// Effective inclusive integer range after folding exclusive bounds into minimum/maximum. A nullopt
// side means that side is unbounded. Decimal strings make this exact beyond int64.
struct EffectiveIntegerRange {
  std::optional<std::string> start;
  std::optional<std::string> end;
};

// Fold the inclusive [minimum, maximum] bounds together with any exclusive bounds so the stricter
// bound wins on each side. Shared by ParseInteger (range validation) and GenerateInteger (grammar
// emission) so the two can never disagree about the effective range. Precondition:
// exclusive_minimum != INT64_MAX and exclusive_maximum != INT64_MIN (ParseInteger rejects those
// before building the spec), so the +1/-1 below cannot overflow.
EffectiveIntegerRange ComputeEffectiveIntegerRange(const IntegerSpec& spec) {
  EffectiveIntegerRange range;
  if (spec.minimum.has_value()) {
    range.start = spec.minimum;
  }
  if (spec.exclusive_minimum.has_value()) {
    std::string excl_start = IncrementInteger(*spec.exclusive_minimum);
    range.start = !range.start.has_value() || CompareCanonicalIntegers(*range.start, excl_start) < 0
                      ? std::optional<std::string>(std::move(excl_start))
                      : range.start;
  }
  if (spec.maximum.has_value()) {
    range.end = spec.maximum;
  }
  if (spec.exclusive_maximum.has_value()) {
    std::string excl_end = DecrementInteger(*spec.exclusive_maximum);
    range.end = !range.end.has_value() || CompareCanonicalIntegers(*range.end, excl_end) > 0
                    ? std::optional<std::string>(std::move(excl_end))
                    : range.end;
  }
  return range;
}

bool TypeSetsOverlap(
    const std::unordered_set<std::string>& lhs, const std::unordered_set<std::string>& rhs
) {
  for (const auto& lhs_type : lhs) {
    for (const auto& rhs_type : rhs) {
      if (lhs_type == rhs_type) {
        return true;
      }
      if ((lhs_type == "integer" || lhs_type == "number") &&
          (rhs_type == "integer" || rhs_type == "number")) {
        return true;
      }
    }
  }
  return false;
}

bool FiniteValuesOverlap(
    const std::vector<picojson::value>& lhs, const std::vector<picojson::value>& rhs
) {
  for (const auto& lhs_value : lhs) {
    for (const auto& rhs_value : rhs) {
      if (JSONValuesMayOverlap(lhs_value, rhs_value)) {
        return true;
      }
    }
  }
  return false;
}

bool FiniteValuesOverlapTypeSet(
    const std::vector<picojson::value>& values, const std::unordered_set<std::string>& type_set
) {
  for (const auto& value : values) {
    if (IsNumericValue(value) && (type_set.count("integer") || type_set.count("number"))) {
      return true;
    }
    for (const auto& type : type_set) {
      if (ValueMatchesType(value, type)) {
        return true;
      }
    }
  }
  return false;
}

bool TryGetFiniteValues(const picojson::object& schema, std::vector<picojson::value>* values) {
  if (schema.count("const")) {
    values->push_back(schema.at("const"));
    return true;
  }
  if (schema.count("enum")) {
    if (!schema.at("enum").is<picojson::array>()) {
      return false;
    }
    const auto& enum_values = schema.at("enum").get<picojson::array>();
    if (enum_values.empty()) {
      return false;
    }
    values->insert(values->end(), enum_values.begin(), enum_values.end());
    return true;
  }
  return false;
}

struct OneOfArmProof {
  enum class Kind { kTypeSet, kFiniteValues };

  Kind kind;
  std::unordered_set<std::string> type_set;
  std::vector<picojson::value> finite_values;
};

std::optional<OneOfArmProof> ClassifyTypeOrFiniteOneOfArm(const picojson::value& option) {
  if (!option.is<picojson::object>()) {
    return std::nullopt;
  }
  const auto& schema = option.get<picojson::object>();

  if (schema.count("$ref") || schema.count("anyOf") || schema.count("allOf") ||
      schema.count("oneOf")) {
    return std::nullopt;
  }

  std::vector<picojson::value> finite_values;
  if (TryGetFiniteValues(schema, &finite_values)) {
    OneOfArmProof proof;
    proof.kind = OneOfArmProof::Kind::kFiniteValues;
    proof.finite_values = std::move(finite_values);
    return proof;
  }

  if (!schema.count("type") || !HasOnlyKeys(schema, {"type"})) {
    return std::nullopt;
  }

  std::unordered_set<std::string> type_set;
  if (!NormalizeTypeSet(schema.at("type"), &type_set)) {
    return std::nullopt;
  }
  if (type_set.count("object")) {
    return std::nullopt;
  }

  OneOfArmProof proof;
  proof.kind = OneOfArmProof::Kind::kTypeSet;
  proof.type_set = std::move(type_set);
  return proof;
}

bool OneOfArmProofsAreDisjoint(const OneOfArmProof& lhs, const OneOfArmProof& rhs) {
  if (lhs.kind == OneOfArmProof::Kind::kTypeSet && rhs.kind == OneOfArmProof::Kind::kTypeSet) {
    return !TypeSetsOverlap(lhs.type_set, rhs.type_set);
  }
  if (lhs.kind == OneOfArmProof::Kind::kFiniteValues &&
      rhs.kind == OneOfArmProof::Kind::kFiniteValues) {
    return !FiniteValuesOverlap(lhs.finite_values, rhs.finite_values);
  }
  if (lhs.kind == OneOfArmProof::Kind::kFiniteValues && rhs.kind == OneOfArmProof::Kind::kTypeSet) {
    return !FiniteValuesOverlapTypeSet(lhs.finite_values, rhs.type_set);
  }
  return !FiniteValuesOverlapTypeSet(rhs.finite_values, lhs.type_set);
}

std::optional<std::vector<picojson::value>> GetDiscriminatorValues(
    const picojson::value& option, const std::string& discriminator_key
) {
  if (!option.is<picojson::object>()) {
    return std::nullopt;
  }
  const auto& schema = option.get<picojson::object>();
  if (schema.count("$ref") || schema.count("anyOf") || schema.count("allOf") ||
      schema.count("oneOf")) {
    return std::nullopt;
  }
  if (!schema.count("type") || !schema.at("type").is<std::string>() ||
      schema.at("type").get<std::string>() != "object") {
    return std::nullopt;
  }
  if (!schema.count("required") || !schema.at("required").is<picojson::array>()) {
    return std::nullopt;
  }

  bool requires_discriminator = false;
  for (const auto& required_key : schema.at("required").get<picojson::array>()) {
    if (!required_key.is<std::string>()) {
      return std::nullopt;
    }
    if (required_key.get<std::string>() == discriminator_key) {
      requires_discriminator = true;
    }
  }
  if (!requires_discriminator) {
    return std::nullopt;
  }

  if (!schema.count("properties") || !schema.at("properties").is<picojson::object>()) {
    return std::nullopt;
  }
  const auto& properties = schema.at("properties").get<picojson::object>();
  auto property_it = properties.find(discriminator_key);
  if (property_it == properties.end() || !property_it->second.is<picojson::object>()) {
    return std::nullopt;
  }

  std::vector<picojson::value> values;
  if (!TryGetFiniteValues(property_it->second.get<picojson::object>(), &values)) {
    return std::nullopt;
  }
  return values;
}

std::vector<std::string> GetDiscriminatorCandidates(const picojson::value& option) {
  std::vector<std::string> candidates;
  if (!option.is<picojson::object>()) {
    return candidates;
  }
  const auto& schema = option.get<picojson::object>();
  if (!schema.count("required") || !schema.at("required").is<picojson::array>() ||
      !schema.count("properties") || !schema.at("properties").is<picojson::object>()) {
    return candidates;
  }
  const auto& properties = schema.at("properties").get<picojson::object>();
  for (const auto& required_key : schema.at("required").get<picojson::array>()) {
    if (!required_key.is<std::string>()) {
      continue;
    }
    const auto& key = required_key.get<std::string>();
    auto property_it = properties.find(key);
    if (property_it == properties.end() || !property_it->second.is<picojson::object>()) {
      continue;
    }
    std::vector<picojson::value> values;
    if (TryGetFiniteValues(property_it->second.get<picojson::object>(), &values)) {
      candidates.push_back(key);
    }
  }
  return candidates;
}

bool TryProveStrictDiscriminatorOneOf(const picojson::array& options) {
  if (options.empty()) {
    return false;
  }

  for (const auto& discriminator_key : GetDiscriminatorCandidates(options.front())) {
    std::vector<std::vector<picojson::value>> branch_values;
    bool all_branches_have_key = true;
    for (const auto& option : options) {
      auto values = GetDiscriminatorValues(option, discriminator_key);
      if (!values.has_value()) {
        all_branches_have_key = false;
        break;
      }
      branch_values.push_back(std::move(values.value()));
    }
    if (!all_branches_have_key) {
      continue;
    }

    bool pairwise_disjoint = true;
    for (size_t i = 0; i < branch_values.size() && pairwise_disjoint; ++i) {
      for (size_t j = i + 1; j < branch_values.size(); ++j) {
        if (FiniteValuesOverlap(branch_values[i], branch_values[j])) {
          pairwise_disjoint = false;
          break;
        }
      }
    }
    if (pairwise_disjoint) {
      return true;
    }
  }
  return false;
}

bool TryProveTypeOrFiniteOneOf(const picojson::array& options) {
  std::vector<OneOfArmProof> proofs;
  proofs.reserve(options.size());
  for (const auto& option : options) {
    auto proof = ClassifyTypeOrFiniteOneOfArm(option);
    if (!proof.has_value()) {
      return false;
    }
    proofs.push_back(std::move(proof.value()));
  }

  for (size_t i = 0; i < proofs.size(); ++i) {
    for (size_t j = i + 1; j < proofs.size(); ++j) {
      if (!OneOfArmProofsAreDisjoint(proofs[i], proofs[j])) {
        return false;
      }
    }
  }
  return true;
}

/*! \brief Rewrite an object oneOf of distinct required keys into pairwise-exclusive objects.
 *
 * A common JSON Schema idiom is
 *
 *   {"properties": {"left": ..., "right": ...},
 *    "additionalProperties": false,
 *    "oneOf": [{"required": ["left"]}, {"required": ["right"]}]}
 *
 * The sibling object keywords apply to every oneOf arm, and an object containing both keys must
 * be rejected because it satisfies both arms.  Each rewritten arm retains all sibling constraints,
 * requires its selected key, and removes the other selected keys from `properties`.  Explicitly
 * disallowing additional properties then makes those removed keys impossible, so the resulting
 * alternatives are pairwise disjoint and exactly preserve this idiom's oneOf semantics.
 */
std::optional<picojson::array> RewriteExclusiveRequiredPropertyOneOf(const picojson::object& schema
) {
  if (!HasOnlyKeys(
          schema,
          {"type",
           "properties",
           "required",
           "additionalProperties",
           "minProperties",
           "maxProperties",
           "oneOf"}
      ) ||
      (schema.count("type") && (!schema.at("type").is<std::string>() ||
                                schema.at("type").get<std::string>() != "object")) ||
      !schema.count("properties") || !schema.at("properties").is<picojson::object>() ||
      !schema.count("additionalProperties") || !schema.at("additionalProperties").is<bool>() ||
      schema.at("additionalProperties").get<bool>() || !schema.count("oneOf") ||
      !schema.at("oneOf").is<picojson::array>()) {
    return std::nullopt;
  }

  const auto& options = schema.at("oneOf").get<picojson::array>();
  if (options.size() < 2) {
    return std::nullopt;
  }

  std::vector<std::string> exclusive_keys;
  exclusive_keys.reserve(options.size());
  std::unordered_set<std::string> seen_keys;
  const auto& properties = schema.at("properties").get<picojson::object>();
  for (const auto& option : options) {
    if (!option.is<picojson::object>()) {
      return std::nullopt;
    }
    const auto& option_object = option.get<picojson::object>();
    if (!HasOnlyKeys(option_object, {"type", "required"}) ||
        (option_object.count("type") && (!option_object.at("type").is<std::string>() ||
                                         option_object.at("type").get<std::string>() != "object")
        ) ||
        !option_object.count("required") || !option_object.at("required").is<picojson::array>()) {
      return std::nullopt;
    }
    const auto& required = option_object.at("required").get<picojson::array>();
    if (required.size() != 1 || !required.front().is<std::string>()) {
      return std::nullopt;
    }
    const std::string& key = required.front().get<std::string>();
    if (!properties.count(key) || !seen_keys.insert(key).second) {
      return std::nullopt;
    }
    exclusive_keys.push_back(key);
  }

  picojson::array outer_required;
  if (schema.count("required")) {
    if (!schema.at("required").is<picojson::array>()) {
      return std::nullopt;
    }
    outer_required = schema.at("required").get<picojson::array>();
    for (const auto& required_key : outer_required) {
      if (!required_key.is<std::string>() || seen_keys.count(required_key.get<std::string>())) {
        return std::nullopt;
      }
    }
  }

  picojson::array rewritten;
  rewritten.reserve(exclusive_keys.size());
  for (const auto& selected_key : exclusive_keys) {
    picojson::object branch = schema;
    branch.erase("oneOf");
    picojson::object branch_properties = properties;
    for (const auto& key : exclusive_keys) {
      if (key != selected_key) {
        branch_properties.erase(key);
      }
    }
    branch["properties"] = picojson::value(std::move(branch_properties));
    picojson::array branch_required = outer_required;
    branch_required.emplace_back(selected_key);
    branch["required"] = picojson::value(std::move(branch_required));
    rewritten.emplace_back(std::move(branch));
  }
  return rewritten;
}

bool TryProvePairwiseDisjointOneOf(const picojson::array& options) {
  return TryProveStrictDiscriminatorOneOf(options) || TryProveTypeOrFiniteOneOf(options);
}

/*!
 * \brief Parser for JSON Schema, converts JSON Schema to SchemaSpec intermediate representation.
 */
class SchemaParser {
 public:
  struct Config {
    bool strict_mode = false;
    JSONFormat json_format;
  };

  explicit SchemaParser(const picojson::value& root_schema, const Config& config)
      : config_(config), root_schema_(root_schema) {}

  Result<SchemaSpecPtr, SchemaError> Parse(
      const picojson::value& schema,
      const std::string& rule_name_hint = "root",
      std::optional<std::string> default_type = std::nullopt
  );

  const picojson::value& GetRootSchema() const { return root_schema_; }
  bool IsStrictMode() const { return config_.strict_mode; }

  Result<SchemaSpecPtr, SchemaError> ResolveRef(
      const std::string& uri, const std::string& rule_name_hint
  );

 private:
  Result<IntegerSpec, SchemaError> ParseInteger(const picojson::object& schema);
  Result<NumberSpec, SchemaError> ParseNumber(const picojson::object& schema);
  Result<StringSpec, SchemaError> ParseString(const picojson::object& schema);
  Result<BooleanSpec, SchemaError> ParseBoolean(const picojson::object& schema);
  Result<NullSpec, SchemaError> ParseNull(const picojson::object& schema);
  Result<ArraySpec, SchemaError> ParseArray(const picojson::object& schema);
  Result<ObjectSpec, SchemaError> ParseObject(const picojson::object& schema);
  Result<ConstSpec, SchemaError> ParseConst(const picojson::object& schema);
  Result<EnumSpec, SchemaError> ParseEnum(const picojson::object& schema);
  Result<RefSpec, SchemaError> ParseRef(const picojson::object& schema);
  Result<AnyOfSpec, SchemaError> ParseAnyOf(
      const picojson::object& schema, const std::string& keyword
  );
  Result<OneOfSpec, SchemaError> ParseOneOf(const picojson::object& schema);
  Result<AllOfSpec, SchemaError> ParseAllOf(const picojson::object& schema);
  Result<TypeArraySpec, SchemaError> ParseTypeArray(
      const picojson::object& schema, const std::string& rule_name_hint
  );

  // Conservatively normalize an allOf whose conjunction is known to describe objects. Returns
  // nullopt when exact merging cannot be proved with the converter's current object model.
  std::optional<picojson::value> TryMergeObjectAllOf(const picojson::object& schema) const;
  // Distribute a conjunction over one nested anyOf/oneOf while retaining the common assertions as
  // siblings of that combinator. ParseAnyOf/ParseOneOf will then push them into every option.
  std::optional<picojson::value> TryDistributeAllOfOverCombinator(const picojson::object& schema
  ) const;
  // Normalize conjunctions that are reduced to non-object primitive or array types. This covers
  // type-specific constraints spread across allOf arms while deliberately rejecting combinations
  // (such as two unrelated patterns) that the current intermediate representation cannot express.
  std::optional<picojson::value> TryMergeTypedAllOf(const picojson::object& schema) const;
  // Conjoin a combinator option with assertions next to the combinator. If the option resolves to
  // another anyOf/oneOf, push the common assertions down through that nested combinator first so
  // each terminal conjunction can be normalized instead of reaching the permissive allOf fallback.
  picojson::value ConjoinWithSiblingAssertions(
      const picojson::value& option, const picojson::object& sibling_assertions
  ) const;
  const picojson::value* ResolveLocalJSONPointer(const std::string& uri) const;

  std::string ComputeCacheKey(const picojson::value& schema);

  static void WarnUnsupportedKeywords(
      const picojson::object& schema, const std::vector<std::string>& keywords, bool verbose = false
  );

  Config config_;
  const picojson::value& root_schema_;
  std::unordered_map<std::string, SchemaSpecPtr> ref_cache_;
  std::unordered_map<std::string, SchemaSpecPtr> schema_cache_;
};

std::string SchemaParser::ComputeCacheKey(const picojson::value& schema) {
  static const std::unordered_set<std::string> kSkippedKeys = {
      "title",
      "default",
      "description",
      "examples",
      "deprecated",
      "readOnly",
      "writeOnly",
      "$comment",
      "$schema",
  };

  if (schema.is<picojson::object>()) {
    std::string result = "{";
    std::vector<std::pair<const std::string*, const picojson::value*>> sorted_kv;
    for (const auto& kv : schema.get<picojson::object>()) {
      if (kSkippedKeys.count(kv.first) == 0) {
        sorted_kv.emplace_back(&kv.first, &kv.second);
      }
    }
    std::sort(sorted_kv.begin(), sorted_kv.end(), [](const auto& lhs, const auto& rhs) {
      return *lhs.first < *rhs.first;
    });
    int64_t idx = 0;
    for (const auto& [key, value] : sorted_kv) {
      if (idx != 0) {
        result += ",";
      }
      ++idx;
      result += "\"" + *key + "\":" + ComputeCacheKey(*value);
    }
    return result + "}";
  } else if (schema.is<picojson::array>()) {
    std::string result = "[";
    int64_t idx = 0;
    for (const auto& item : schema.get<picojson::array>()) {
      if (idx != 0) {
        result += ",";
      }
      ++idx;
      result += ComputeCacheKey(item);
    }
    return result + "]";
  }
  return schema.serialize(false);
}

void SchemaParser::WarnUnsupportedKeywords(
    const picojson::object& schema, const std::vector<std::string>& keywords, bool verbose
) {
  if (!verbose) {
    return;
  }
  for (const auto& keyword : keywords) {
    if (schema.find(keyword) != schema.end()) {
      XGRAMMAR_LOG(WARNING) << "Keyword " << keyword << " is not supported";
    }
  }
}

const picojson::value* SchemaParser::ResolveLocalJSONPointer(const std::string& uri) const {
  if (uri == "#") {
    return &root_schema_;
  }
  if (uri.empty() || uri[0] != '#') {
    return nullptr;
  }

  // Plain-name fragments identify either a modern $anchor/$dynamicAnchor or, in older drafts,
  // a subschema whose id is that fragment. Draft-04 schemas using this form are still common in
  // the wild, and treating the reference as unconstrained silently drops every assertion below
  // it. Search the schema tree because anchors identify schema resources rather than JSON Pointer
  // paths.
  if (uri.size() >= 2 && uri[1] != '/') {
    const std::string anchor = uri.substr(1);
    std::function<const picojson::value*(const picojson::value&)> find_anchor;
    find_anchor = [&](const picojson::value& value) -> const picojson::value* {
      if (value.is<picojson::object>()) {
        const auto& object = value.get<picojson::object>();
        for (const char* keyword : {"$anchor", "$dynamicAnchor"}) {
          auto it = object.find(keyword);
          if (it != object.end() && it->second.is<std::string>() &&
              it->second.get<std::string>() == anchor) {
            return &value;
          }
        }
        for (const char* keyword : {"id", "$id"}) {
          auto it = object.find(keyword);
          if (it != object.end() && it->second.is<std::string>() &&
              it->second.get<std::string>() == uri) {
            return &value;
          }
        }
        for (const auto& [_, child] : object) {
          if (const auto* found = find_anchor(child)) {
            return found;
          }
        }
      } else if (value.is<picojson::array>()) {
        for (const auto& child : value.get<picojson::array>()) {
          if (const auto* found = find_anchor(child)) {
            return found;
          }
        }
      }
      return nullptr;
    };
    return find_anchor(root_schema_);
  }

  if (uri.size() < 2 || uri[1] != '/') {
    return nullptr;
  }
  const picojson::value* current = &root_schema_;
  size_t begin = 2;
  while (true) {
    size_t slash = uri.find('/', begin);
    std::string token = uri.substr(begin, slash == std::string::npos ? slash : slash - begin);
    std::string decoded;
    decoded.reserve(token.size());
    for (size_t i = 0; i < token.size(); ++i) {
      if (token[i] == '~' && i + 1 < token.size() && (token[i + 1] == '0' || token[i + 1] == '1')) {
        decoded.push_back(token[++i] == '0' ? '~' : '/');
      } else {
        decoded.push_back(token[i]);
      }
    }
    if (current->is<picojson::object>()) {
      if (!current->contains(decoded)) {
        return nullptr;
      }
      current = &current->get(decoded);
    } else if (current->is<picojson::array>()) {
      if (decoded.empty()) {
        return nullptr;
      }
      size_t index = 0;
      for (char c : decoded) {
        if (c < '0' || c > '9' ||
            index > (std::numeric_limits<size_t>::max() - static_cast<size_t>(c - '0')) / 10) {
          return nullptr;
        }
        index = index * 10 + static_cast<size_t>(c - '0');
      }
      const auto& array = current->get<picojson::array>();
      if (index >= array.size()) {
        return nullptr;
      }
      current = &array[index];
    } else {
      return nullptr;
    }
    if (slash == std::string::npos) {
      return current;
    }
    begin = slash + 1;
  }
}

std::optional<picojson::value> SchemaParser::TryMergeObjectAllOf(const picojson::object& schema
) const {
  struct ObjectConjunct {
    picojson::object schema;
    std::unordered_set<std::string> named_properties;
    bool additional_properties_false = false;
  };

  std::vector<ObjectConjunct> conjuncts;
  std::unordered_set<std::string> active_refs;
  bool has_object_type = false;
  bool has_false_schema = false;
  bool has_non_object_type = false;
  bool has_non_object_assertion = false;

  auto has_semantic_sibling = [](const picojson::object& object) {
    static const std::unordered_set<std::string> kAnnotations = {
        "$id",
        "id",
        "$schema",
        "$defs",
        "definitions",
        "title",
        "default",
        "description",
        "examples",
        "deprecated",
        "readOnly",
        "writeOnly",
        "$comment",
        "example",
    };
    return std::any_of(object.begin(), object.end(), [&](const auto& item) {
      return kAnnotations.count(item.first) == 0;
    });
  };

  std::function<bool(const picojson::value&, int)> collect;
  collect = [&](const picojson::value& value, int depth) -> bool {
    if (depth > 64) {
      return false;
    }
    if (value.is<bool>()) {
      has_false_schema = has_false_schema || !value.get<bool>();
      return true;
    }
    if (!value.is<picojson::object>()) {
      return false;
    }
    picojson::object object = value.get<picojson::object>();

    if (object.count("$ref")) {
      if (!object.at("$ref").is<std::string>()) {
        return false;
      }
      picojson::object siblings = object;
      const std::string uri = siblings.at("$ref").get<std::string>();
      siblings.erase("$ref");
      if (has_semantic_sibling(siblings)) {
        // The meaning of siblings next to $ref depends on the Schema draft. The existing parser
        // follows the older replacement semantics, so do not silently change that behavior here.
        return false;
      }
      const picojson::value* target = ResolveLocalJSONPointer(uri);
      if (target == nullptr || active_refs.count(uri)) {
        return false;
      }
      active_refs.insert(uri);
      bool success = collect(*target, depth + 1);
      active_refs.erase(uri);
      return success;
    }

    if (object.count("allOf")) {
      if (!object.at("allOf").is<picojson::array>()) {
        return false;
      }
      picojson::array nested = object.at("allOf").get<picojson::array>();
      object.erase("allOf");
      if (has_semantic_sibling(object) && !collect(picojson::value(object), depth + 1)) {
        return false;
      }
      for (const auto& child : nested) {
        if (!collect(child, depth + 1)) {
          return false;
        }
      }
      return true;
    }

    if (object.count("type")) {
      if (!object.at("type").is<std::string>()) {
        return false;
      }
      const std::string& type = object.at("type").get<std::string>();
      if (type != "object") {
        has_non_object_type = true;
        return true;
      }
      has_object_type = true;
    }

    static const std::unordered_set<std::string> kUnsupportedObjectConjunctionKeywords = {
        "anyOf",
        "oneOf",
        "not",
        "if",
        "then",
        "else",
        "dependentRequired",
        "dependentSchemas",
        "dependencies",
        "contains",
        "const",
        "enum",
    };
    if (std::any_of(object.begin(), object.end(), [&](const auto& item) {
          return kUnsupportedObjectConjunctionKeywords.count(item.first) != 0;
        })) {
      return false;
    }
    if (object.count("patternProperties") &&
        !object.at("patternProperties").is<picojson::object>()) {
      return false;
    }
    if (object.count("unevaluatedProperties") &&
        (!object.at("unevaluatedProperties").is<bool>() ||
         !object.at("unevaluatedProperties").get<bool>())) {
      return false;
    }
    static const std::unordered_set<std::string> kObjectConjunctionKeywords = {
        "type",
        "properties",
        "required",
        "additionalProperties",
        "unevaluatedProperties",
        "patternProperties",
        "propertyNames",
        "minProperties",
        "maxProperties",
    };
    for (const auto& [keyword, value] : object) {
      static_cast<void>(value);
      if (IsKnownSchemaAssertionKeyword(keyword) && !kObjectConjunctionKeywords.count(keyword)) {
        has_non_object_assertion = true;
      }
    }

    ObjectConjunct conjunct{std::move(object), {}, false};
    if (conjunct.schema.count("properties")) {
      if (!conjunct.schema.at("properties").is<picojson::object>()) {
        return false;
      }
      for (const auto& item : conjunct.schema.at("properties").get<picojson::object>()) {
        conjunct.named_properties.insert(item.first);
      }
    }
    if (conjunct.schema.count("additionalProperties")) {
      const auto& additional = conjunct.schema.at("additionalProperties");
      if (!additional.is<bool>()) {
        return false;
      }
      conjunct.additional_properties_false = !additional.get<bool>();
    } else if (config_.strict_mode) {
      // Preserve the converter's documented strict-mode narrowing when several object branches
      // are merged. In standards mode the JSON Schema default remains true.
      conjunct.additional_properties_false = true;
    }
    conjuncts.push_back(std::move(conjunct));
    return true;
  };

  if (!collect(picojson::value(schema), 0)) {
    return std::nullopt;
  }
  if (has_false_schema || (has_object_type && has_non_object_type)) {
    return picojson::value(false);
  }
  std::unordered_set<std::string> all_named_properties;
  std::optional<picojson::value> pattern_properties;
  for (const auto& conjunct : conjuncts) {
    all_named_properties.insert(conjunct.named_properties.begin(), conjunct.named_properties.end());
    if (conjunct.schema.count("patternProperties") &&
        !conjunct.schema.at("patternProperties").get<picojson::object>().empty()) {
      const auto& candidate = conjunct.schema.at("patternProperties");
      if (!pattern_properties.has_value()) {
        pattern_properties = candidate;
      } else if (pattern_properties->serialize(false) != candidate.serialize(false)) {
        // Combining distinct pattern sets can make one key match schemas from separate
        // conjuncts, which requires a general schema intersection. An identical set (or a set
        // present in only one conjunct) can be carried through unchanged.
        return std::nullopt;
      }
    }
  }
  for (const auto& conjunct : conjuncts) {
    if (conjunct.additional_properties_false && conjunct.named_properties != all_named_properties) {
      // A property introduced only by another branch is additional in this branch and therefore
      // forbidden. The current ObjectSpec has only one global property set, so it cannot encode
      // that distinction exactly.
      return std::nullopt;
    }
    if (conjunct.additional_properties_false && pattern_properties.has_value()) {
      if (!conjunct.schema.count("patternProperties") ||
          conjunct.schema.at("patternProperties").serialize(false) !=
              pattern_properties->serialize(false)) {
        // A pattern introduced only by another conjunct is still an additional property in this
        // closed conjunct. Moving it into one merged ObjectSpec would incorrectly admit the key.
        return std::nullopt;
      }
    }
  }

  picojson::object merged;
  merged["type"] = picojson::value("object");
  picojson::object merged_properties;
  picojson::array merged_required;
  std::unordered_set<std::string> seen_required;
  std::optional<picojson::value> property_names;
  int64_t min_properties = 0;
  std::optional<int64_t> max_properties;
  bool additional_properties_false = false;

  for (const auto& conjunct : conjuncts) {
    if (conjunct.schema.count("properties")) {
      for (const auto& item : conjunct.schema.at("properties").get<picojson::object>()) {
        auto existing = merged_properties.find(item.first);
        if (existing == merged_properties.end()) {
          merged_properties[item.first] = item.second;
        } else if (existing->second.serialize(false) != item.second.serialize(false)) {
          picojson::array property_conjunction;
          property_conjunction.push_back(existing->second);
          property_conjunction.push_back(item.second);
          picojson::object property_schema;
          property_schema["allOf"] = picojson::value(std::move(property_conjunction));
          auto merged_property = TryMergeObjectAllOf(property_schema);
          if (!merged_property.has_value()) {
            merged_property = TryMergeTypedAllOf(property_schema);
          }
          if (!merged_property.has_value()) {
            // Feeding an unsupported child allOf back to Parse would eventually reach
            // GenerateAllOf's permissive fallback and make the outer merge unsound.
            return std::nullopt;
          }
          existing->second = std::move(*merged_property);
        }
      }
    }
    if (conjunct.schema.count("required")) {
      if (!conjunct.schema.at("required").is<picojson::array>()) {
        return std::nullopt;
      }
      for (const auto& required : conjunct.schema.at("required").get<picojson::array>()) {
        if (!required.is<std::string>()) {
          return std::nullopt;
        }
        if (seen_required.insert(required.get<std::string>()).second) {
          merged_required.push_back(required);
        }
      }
    }
    if (conjunct.schema.count("propertyNames")) {
      if (!property_names.has_value()) {
        property_names = conjunct.schema.at("propertyNames");
      } else if (property_names->serialize(false) !=
                 conjunct.schema.at("propertyNames").serialize(false)) {
        return std::nullopt;
      }
    }
    if (conjunct.schema.count("minProperties")) {
      if (!conjunct.schema.at("minProperties").is<int64_t>()) {
        return std::nullopt;
      }
      min_properties = std::max(min_properties, conjunct.schema.at("minProperties").get<int64_t>());
    }
    if (conjunct.schema.count("maxProperties")) {
      if (!conjunct.schema.at("maxProperties").is<int64_t>()) {
        return std::nullopt;
      }
      int64_t value = conjunct.schema.at("maxProperties").get<int64_t>();
      max_properties = max_properties.has_value() ? std::min(*max_properties, value) : value;
    }
    additional_properties_false =
        additional_properties_false || conjunct.additional_properties_false;
  }

  if (!merged_properties.empty()) {
    merged["properties"] = picojson::value(std::move(merged_properties));
  }
  if (!merged_required.empty()) {
    merged["required"] = picojson::value(std::move(merged_required));
  }
  if (property_names.has_value()) {
    merged["propertyNames"] = std::move(*property_names);
  }
  if (pattern_properties.has_value()) {
    // Keep the local copy available for the closed-object required-name satisfiability check
    // below. picojson's moved-from value no longer retains the pattern object.
    merged["patternProperties"] = *pattern_properties;
  }
  if (min_properties != 0) {
    merged["minProperties"] = picojson::value(min_properties);
  }
  if (max_properties.has_value()) {
    if (*max_properties >= 0 && (min_properties > *max_properties ||
                                 static_cast<int64_t>(seen_required.size()) > *max_properties)) {
      return picojson::value(false);
    }
    merged["maxProperties"] = picojson::value(*max_properties);
  }
  if (additional_properties_false) {
    std::unordered_map<std::string, FSMWithStartEnd> pattern_fsms;
    std::unordered_set<std::string> failed_pattern_fsms;
    auto pattern_matches_property = [&](const std::string& pattern,
                                        const std::string& property) -> std::optional<bool> {
      auto cached = pattern_fsms.find(pattern);
      if (cached == pattern_fsms.end() && !failed_pattern_fsms.count(pattern)) {
        auto built = BuildJSONSchemaPatternFSM(pattern, kJSONSchemaPatternDFAStateLimit);
        if (built.IsOk()) {
          cached = pattern_fsms.emplace(pattern, std::move(built).Unwrap()).first;
        } else {
          failed_pattern_fsms.insert(pattern);
        }
      }
      if (cached == pattern_fsms.end()) {
        return std::nullopt;
      }
      return cached->second.AcceptString(property);
    };

    for (const auto& name : seen_required) {
      if (all_named_properties.count(name)) {
        continue;
      }

      bool admitted_by_pattern = false;
      bool pattern_match_unknown = false;
      if (pattern_properties.has_value()) {
        const auto& patterns = pattern_properties->get<picojson::object>();
        for (const auto& pattern : patterns.ordered_keys()) {
          auto matches = pattern_matches_property(pattern, name);
          if (!matches.has_value()) {
            pattern_match_unknown = true;
          } else if (*matches) {
            admitted_by_pattern = true;
          }
        }
      }
      if (admitted_by_pattern) {
        continue;
      }
      if (pattern_match_unknown) {
        // A failed pattern construction means that the fixed required name might still be
        // admitted. Abandon this optimization instead of declaring a satisfiable branch empty.
        return std::nullopt;
      }
      return picojson::value(false);
    }
    merged["additionalProperties"] = picojson::value(false);
  }
  if (!has_object_type) {
    if (has_non_object_type || has_non_object_assertion) {
      return std::nullopt;
    }
    // Object keywords are conditional: a conjunction such as
    // `allOf: [{required: ["name"]}, {properties: {...}}]` accepts every non-object value and
    // applies the merged constraints only to objects. Preserve both halves explicitly instead of
    // falling through to GenerateAllOf's unconstrained fallback.
    picojson::array non_object_types;
    for (const char* type : {"null", "boolean", "array", "string", "number"}) {
      non_object_types.emplace_back(type);
    }
    picojson::object non_object_schema;
    non_object_schema["type"] = picojson::value(std::move(non_object_types));
    picojson::array alternatives;
    alternatives.emplace_back(std::move(merged));
    alternatives.emplace_back(std::move(non_object_schema));
    picojson::object conditional_object;
    conditional_object["anyOf"] = picojson::value(std::move(alternatives));
    return picojson::value(std::move(conditional_object));
  }
  return picojson::value(std::move(merged));
}

std::optional<picojson::value> SchemaParser::TryDistributeAllOfOverCombinator(
    const picojson::object& schema
) const {
  auto all_of_it = schema.find("allOf");
  if (all_of_it == schema.end() || !all_of_it->second.is<picojson::array>()) {
    return std::nullopt;
  }

  std::optional<std::string> distributed_keyword;
  picojson::value distributed_options;
  picojson::array common_conjuncts;
  std::unordered_set<std::string> active_refs;

  auto has_assertion = [](const picojson::object& object) {
    return std::any_of(object.begin(), object.end(), [](const auto& item) {
      return !IsSchemaAnnotationKey(item.first) && IsKnownSchemaAssertionKeyword(item.first);
    });
  };

  std::function<bool(const picojson::value&, int)> collect;
  collect = [&](const picojson::value& value, int depth) -> bool {
    if (depth > 64) return false;
    if (!value.is<picojson::object>()) {
      common_conjuncts.push_back(value);
      return true;
    }

    picojson::object object = value.get<picojson::object>();
    if (object.count("$ref")) {
      if (!object.at("$ref").is<std::string>()) {
        common_conjuncts.push_back(value);
        return true;
      }
      const std::string& uri = object.at("$ref").get<std::string>();
      const picojson::value* target = ResolveLocalJSONPointer(uri);
      if (target == nullptr || !active_refs.insert(uri).second) {
        common_conjuncts.push_back(value);
        return true;
      }
      bool success = collect(*target, depth + 1);
      active_refs.erase(uri);
      return success;
    }

    // Match Parse()'s keyword precedence. Once one combinator has been selected for distribution,
    // leave later combinators in the common conjunction; a subsequent Parse(allOf) can distribute
    // those in turn.
    const char* keyword = nullptr;
    if (!object.count("const") && !object.count("enum")) {
      if (object.count("anyOf")) {
        keyword = "anyOf";
      } else if (object.count("oneOf")) {
        keyword = "oneOf";
      }
    }
    if (keyword != nullptr && !distributed_keyword.has_value()) {
      if (!object.at(keyword).is<picojson::array>()) return false;
      distributed_keyword = keyword;
      distributed_options = object.at(keyword);
      object.erase(keyword);
      if (has_assertion(object)) common_conjuncts.emplace_back(std::move(object));
      return true;
    }

    if (!object.count("const") && !object.count("enum") && !object.count("anyOf") &&
        !object.count("oneOf") && object.count("allOf")) {
      if (!object.at("allOf").is<picojson::array>()) return false;
      picojson::array nested = object.at("allOf").get<picojson::array>();
      object.erase("allOf");
      if (has_assertion(object)) common_conjuncts.emplace_back(std::move(object));
      for (const auto& child : nested) {
        if (!collect(child, depth + 1)) return false;
      }
      return true;
    }

    if (has_assertion(object)) common_conjuncts.emplace_back(std::move(object));
    return true;
  };

  picojson::object root_siblings = schema;
  root_siblings.erase("allOf");
  if (has_assertion(root_siblings)) common_conjuncts.emplace_back(std::move(root_siblings));
  for (const auto& child : all_of_it->second.get<picojson::array>()) {
    if (!collect(child, 0)) return std::nullopt;
  }
  if (!distributed_keyword.has_value()) return std::nullopt;

  picojson::object distributed;
  distributed[*distributed_keyword] = std::move(distributed_options);
  if (!common_conjuncts.empty()) {
    distributed["allOf"] = picojson::value(std::move(common_conjuncts));
  }
  return picojson::value(std::move(distributed));
}

std::optional<picojson::value> SchemaParser::TryMergeTypedAllOf(const picojson::object& schema
) const {
  // JSON Schema's primitive keywords are conditional on the instance type. Once the conjunction
  // contains an explicit type restriction, constraints for impossible types can be discarded and
  // the remaining bounds can be combined without constructing a grammar-language intersection.
  // Use separate bits for integers and non-integer numbers because `number` contains both.
  constexpr uint32_t kNull = 1u << 0;
  constexpr uint32_t kBoolean = 1u << 1;
  constexpr uint32_t kObject = 1u << 2;
  constexpr uint32_t kArray = 1u << 3;
  constexpr uint32_t kString = 1u << 4;
  constexpr uint32_t kInteger = 1u << 5;
  constexpr uint32_t kNonIntegerNumber = 1u << 6;

  auto type_bits = [&](const picojson::value& value) -> std::optional<uint32_t> {
    auto one_type = [&](const std::string& type) -> std::optional<uint32_t> {
      if (type == "null") return kNull;
      if (type == "boolean") return kBoolean;
      if (type == "object") return kObject;
      if (type == "array") return kArray;
      if (type == "string") return kString;
      if (type == "integer") return kInteger;
      if (type == "number") return kInteger | kNonIntegerNumber;
      return std::nullopt;
    };

    if (value.is<std::string>()) {
      return one_type(value.get<std::string>());
    }
    if (!value.is<picojson::array>() || value.get<picojson::array>().empty()) {
      return std::nullopt;
    }
    uint32_t result = 0;
    for (const auto& item : value.get<picojson::array>()) {
      if (!item.is<std::string>()) return std::nullopt;
      auto item_bits = one_type(item.get<std::string>());
      if (!item_bits.has_value()) return std::nullopt;
      result |= *item_bits;
    }
    return result;
  };

  auto is_annotation = [](const std::string& keyword) {
    static const std::unordered_set<std::string> kAnnotations = {
        "$comment",
        "$id",
        "$schema",
        "$defs",
        "definitions",
        "default",
        "deprecated",
        "description",
        "examples",
        "example",
        "id",
        "readOnly",
        "title",
        "writeOnly",
    };
    return kAnnotations.count(keyword) != 0;
  };
  auto has_assertion = [&](const picojson::object& object) {
    return std::any_of(object.begin(), object.end(), [&](const auto& item) {
      return !is_annotation(item.first);
    });
  };

  std::vector<picojson::object> conjuncts;
  std::unordered_set<std::string> active_refs;
  bool is_never = false;
  std::function<bool(const picojson::value&, int)> collect;
  collect = [&](const picojson::value& value, int depth) -> bool {
    if (depth > 64) return false;
    if (value.is<bool>()) {
      is_never = is_never || !value.get<bool>();
      return true;
    }
    if (!value.is<picojson::object>()) return false;
    picojson::object object = value.get<picojson::object>();

    if (object.count("$ref")) {
      if (!object.at("$ref").is<std::string>()) return false;
      // Match the parser's existing draft-04-compatible replacement semantics: siblings of $ref
      // are not assertions in that mode.
      const std::string uri = object.at("$ref").get<std::string>();
      const picojson::value* target = ResolveLocalJSONPointer(uri);
      if (target == nullptr || active_refs.count(uri)) return false;
      active_refs.insert(uri);
      bool success = collect(*target, depth + 1);
      active_refs.erase(uri);
      return success;
    }

    if (object.count("allOf")) {
      if (!object.at("allOf").is<picojson::array>()) return false;
      picojson::array nested = object.at("allOf").get<picojson::array>();
      object.erase("allOf");
      if (has_assertion(object) && !collect(picojson::value(std::move(object)), depth + 1)) {
        return false;
      }
      for (const auto& child : nested) {
        if (!collect(child, depth + 1)) return false;
      }
      return true;
    }

    conjuncts.push_back(std::move(object));
    return true;
  };

  if (!collect(picojson::value(schema), 0)) return std::nullopt;
  if (is_never) return picojson::value(false);

  std::optional<uint32_t> allowed_types;
  for (const auto& conjunct : conjuncts) {
    auto type_it = conjunct.find("type");
    if (type_it == conjunct.end()) continue;
    auto bits = type_bits(type_it->second);
    if (!bits.has_value()) return std::nullopt;
    allowed_types = allowed_types.has_value() ? (*allowed_types & *bits) : *bits;
  }
  if (!allowed_types.has_value()) {
    // Without a type restriction, different primitive keywords apply to different disjoint
    // portions of the JSON value space. A single typed spec cannot represent that union.
    return std::nullopt;
  }
  if (*allowed_types == 0) return picojson::value(false);
  if ((*allowed_types & kNonIntegerNumber) && !(*allowed_types & kInteger)) {
    // The type vocabulary cannot express "number but not integer". Intersections of positive type
    // assertions do not normally create this set, so retain a conservative guard.
    return std::nullopt;
  }

  auto keyword_applies = [&](const std::string& keyword) {
    static const std::unordered_set<std::string> kNumberKeywords = {
        "exclusiveMaximum", "exclusiveMinimum", "maximum", "minimum", "multipleOf"
    };
    static const std::unordered_set<std::string> kStringKeywords = {
        "contentEncoding", "contentMediaType", "format", "maxLength", "minLength", "pattern"
    };
    static const std::unordered_set<std::string> kArrayKeywords = {
        "additionalItems",
        "contains",
        "items",
        "maxContains",
        "maxItems",
        "minContains",
        "minItems",
        "prefixItems",
        "unevaluatedItems",
        "uniqueItems",
    };
    static const std::unordered_set<std::string> kObjectKeywords = {
        "additionalProperties",
        "dependencies",
        "dependentRequired",
        "dependentSchemas",
        "maxProperties",
        "minProperties",
        "patternProperties",
        "properties",
        "propertyNames",
        "required",
        "unevaluatedProperties",
    };
    if (kNumberKeywords.count(keyword)) {
      return (*allowed_types & (kInteger | kNonIntegerNumber)) != 0;
    }
    if (kStringKeywords.count(keyword)) return (*allowed_types & kString) != 0;
    if (kArrayKeywords.count(keyword)) return (*allowed_types & kArray) != 0;
    if (kObjectKeywords.count(keyword)) return (*allowed_types & kObject) != 0;
    return true;
  };

  picojson::object merged;
  std::optional<picojson::array> finite_values;
  bool has_non_type_assertion = false;
  auto merge_finite_values = [&](picojson::array candidates) {
    if (!finite_values.has_value()) {
      finite_values = std::move(candidates);
      return;
    }
    picojson::array intersection;
    for (const auto& value : *finite_values) {
      if (std::any_of(candidates.begin(), candidates.end(), [&](const picojson::value& candidate) {
            return JSONValuesMayOverlap(value, candidate);
          })) {
        intersection.push_back(value);
      }
    }
    finite_values = std::move(intersection);
  };

  for (const auto& conjunct : conjuncts) {
    for (const auto& [keyword, value] : conjunct) {
      if (keyword == "type" || is_annotation(keyword)) continue;
      // JSON Schema vocabularies define unknown extension keywords as annotations unless a custom
      // vocabulary assigns them assertion semantics. Match the converter's existing behavior for
      // fields such as `readonly`, `mergeStrategy`, and application metadata.
      if (!IsKnownSchemaAssertionKeyword(keyword)) continue;
      if (!keyword_applies(keyword)) continue;

      if (keyword == "const") {
        merge_finite_values(picojson::array{value});
        continue;
      }
      if (keyword == "enum") {
        if (!value.is<picojson::array>() || value.get<picojson::array>().empty()) {
          return std::nullopt;
        }
        merge_finite_values(value.get<picojson::array>());
        continue;
      }

      static const std::unordered_set<std::string> kSupportedKeywords = {
          "exclusiveMaximum",
          "exclusiveMinimum",
          "maximum",
          "minimum",
          "multipleOf",
          "format",
          "maxLength",
          "minLength",
          "pattern",
          "additionalItems",
          "items",
          "maxItems",
          "minItems",
          "prefixItems",
          "unevaluatedItems",
      };
      if (!kSupportedKeywords.count(keyword)) return std::nullopt;
      has_non_type_assertion = true;

      auto existing = merged.find(keyword);
      if (existing == merged.end()) {
        merged[keyword] = value;
        continue;
      }
      if (existing->second.serialize(false) == value.serialize(false)) continue;

      if (keyword == "minLength" || keyword == "minItems") {
        if (!existing->second.is<int64_t>() || !value.is<int64_t>()) return std::nullopt;
        existing->second =
            picojson::value(std::max(existing->second.get<int64_t>(), value.get<int64_t>()));
      } else if (keyword == "maxLength" || keyword == "maxItems") {
        if (!existing->second.is<int64_t>() || !value.is<int64_t>()) return std::nullopt;
        existing->second =
            picojson::value(std::min(existing->second.get<int64_t>(), value.get<int64_t>()));
      } else {
        // Multiple different regexes, formats, item schemas, numeric bounds with the same keyword,
        // or multipleOf values require a richer intersection representation.
        return std::nullopt;
      }
    }
  }

  if (finite_values.has_value()) {
    if (has_non_type_assertion) return std::nullopt;
    picojson::array filtered;
    for (const auto& value : *finite_values) {
      bool matches = ((*allowed_types & kNull) && ValueMatchesType(value, "null")) ||
                     ((*allowed_types & kBoolean) && ValueMatchesType(value, "boolean")) ||
                     ((*allowed_types & kObject) && ValueMatchesType(value, "object")) ||
                     ((*allowed_types & kArray) && ValueMatchesType(value, "array")) ||
                     ((*allowed_types & kString) && ValueMatchesType(value, "string")) ||
                     ((*allowed_types & kInteger) && ValueMatchesType(value, "integer")) ||
                     ((*allowed_types & kNonIntegerNumber) && ValueMatchesType(value, "number") &&
                      !ValueMatchesType(value, "integer"));
      if (matches) filtered.push_back(value);
    }
    if (filtered.empty()) return picojson::value(false);
    picojson::object finite_schema;
    finite_schema["enum"] = picojson::value(std::move(filtered));
    return picojson::value(std::move(finite_schema));
  }

  auto incompatible_bounds = [&](const char* minimum, const char* maximum) {
    auto min_it = merged.find(minimum);
    auto max_it = merged.find(maximum);
    return min_it != merged.end() && max_it != merged.end() && min_it->second.is<int64_t>() &&
           max_it->second.is<int64_t>() &&
           min_it->second.get<int64_t>() > max_it->second.get<int64_t>();
  };
  if (incompatible_bounds("minLength", "maxLength") ||
      incompatible_bounds("minItems", "maxItems")) {
    return picojson::value(false);
  }

  picojson::array output_types;
  if (*allowed_types & kNull) output_types.emplace_back("null");
  if (*allowed_types & kBoolean) output_types.emplace_back("boolean");
  if (*allowed_types & kObject) output_types.emplace_back("object");
  if (*allowed_types & kArray) output_types.emplace_back("array");
  if (*allowed_types & kString) output_types.emplace_back("string");
  if ((*allowed_types & (kInteger | kNonIntegerNumber)) == (kInteger | kNonIntegerNumber)) {
    output_types.emplace_back("number");
  } else if (*allowed_types & kInteger) {
    output_types.emplace_back("integer");
  }
  XGRAMMAR_DCHECK(!output_types.empty());
  merged["type"] =
      output_types.size() == 1 ? output_types.front() : picojson::value(std::move(output_types));
  return picojson::value(std::move(merged));
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::Parse(
    const picojson::value& schema,
    const std::string& rule_name_hint,
    std::optional<std::string> default_type
) {
  std::string cache_key = ComputeCacheKey(schema);
  // The same schema object can be parsed both as an ordinary schema and in a typed context (most
  // notably propertyNames, whose instance is necessarily a string). These are different
  // semantics, so they must not alias in either the parser cache or the generated-rule cache.
  if (default_type.has_value()) {
    cache_key += "\0default-type:" + *default_type;
  }
  if (schema_cache_.count(cache_key)) {
    return ResultOk(schema_cache_[cache_key]);
  }

  if (schema.is<bool>()) {
    auto spec = schema.get<bool>() ? SchemaSpec::Make(AnySpec{}, cache_key, rule_name_hint)
                                   : SchemaSpec::Make(NeverSpec{}, cache_key, rule_name_hint);
    schema_cache_[cache_key] = spec;
    return ResultOk(spec);
  }

  if (!schema.is<picojson::object>()) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema,
        "Schema should be an object or bool, but got " + schema.serialize(false)
    );
  }

  const auto& schema_obj = schema.get<picojson::object>();
  WarnUnsupportedKeywords(
      schema_obj, {"not", "if", "then", "else", "dependentRequired", "dependentSchemas"}
  );

  SchemaSpecPtr result;

  // `const` and `enum` are checked before `type` below. Filter their finite value set first so a
  // sibling type assertion cannot be bypassed by a differently typed finite value.
  if ((schema_obj.count("const") || schema_obj.count("enum")) && schema_obj.count("type")) {
    if (auto merged = TryMergeTypedAllOf(schema_obj)) {
      auto merged_result = Parse(*merged, rule_name_hint);
      if (merged_result.IsErr()) return ResultErr(std::move(merged_result).UnwrapErr());
      result = std::move(merged_result).Unwrap();
      schema_cache_[cache_key] = result;
      return ResultOk(result);
    }
  }

  if (schema_obj.count("$ref")) {
    auto ref_result = ParseRef(schema_obj);
    if (ref_result.IsErr()) return ResultErr(std::move(ref_result).UnwrapErr());
    auto ref_spec = std::move(ref_result).Unwrap();
    result = SchemaSpec::Make(std::move(ref_spec), cache_key, rule_name_hint);
  } else if (schema_obj.count("const")) {
    auto const_result = ParseConst(schema_obj);
    if (const_result.IsErr()) return ResultErr(std::move(const_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(const_result).Unwrap(), cache_key, rule_name_hint);
  } else if (schema_obj.count("enum")) {
    auto enum_result = ParseEnum(schema_obj);
    if (enum_result.IsErr()) return ResultErr(std::move(enum_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(enum_result).Unwrap(), cache_key, rule_name_hint);
  } else if (schema_obj.count("anyOf")) {
    auto anyof_result = ParseAnyOf(schema_obj, "anyOf");
    if (anyof_result.IsErr()) return ResultErr(std::move(anyof_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(anyof_result).Unwrap(), cache_key, rule_name_hint);
  } else if (schema_obj.count("oneOf")) {
    auto oneof_result = ParseOneOf(schema_obj);
    if (oneof_result.IsErr()) {
      if (oneof_result.ErrRef().Type() != SchemaErrorType::kUnsupportedSchema) {
        return ResultErr(std::move(oneof_result).UnwrapErr());
      }
      XGRAMMAR_LOG(WARNING) << oneof_result.ErrRef().what();
      auto anyof_result = ParseAnyOf(schema_obj, "oneOf");
      if (anyof_result.IsErr()) return ResultErr(std::move(anyof_result).UnwrapErr());
      result = SchemaSpec::Make(std::move(anyof_result).Unwrap(), cache_key, rule_name_hint);
    } else {
      result = SchemaSpec::Make(std::move(oneof_result).Unwrap(), cache_key, rule_name_hint);
    }
  } else if (schema_obj.count("allOf")) {
    if (auto distributed = TryDistributeAllOfOverCombinator(schema_obj)) {
      auto distributed_result = Parse(*distributed, rule_name_hint);
      if (distributed_result.IsErr()) {
        return ResultErr(std::move(distributed_result).UnwrapErr());
      }
      result = std::move(distributed_result).Unwrap();
      schema_cache_[cache_key] = result;
      return ResultOk(result);
    }
    if (auto merged = TryMergeObjectAllOf(schema_obj)) {
      auto merged_result = Parse(*merged, rule_name_hint);
      if (merged_result.IsErr()) return ResultErr(std::move(merged_result).UnwrapErr());
      result = std::move(merged_result).Unwrap();
      schema_cache_[cache_key] = result;
      return ResultOk(result);
    }
    if (auto merged = TryMergeTypedAllOf(schema_obj)) {
      auto merged_result = Parse(*merged, rule_name_hint);
      if (merged_result.IsErr()) return ResultErr(std::move(merged_result).UnwrapErr());
      result = std::move(merged_result).Unwrap();
      schema_cache_[cache_key] = result;
      return ResultOk(result);
    }
    auto allof_result = ParseAllOf(schema_obj);
    if (allof_result.IsErr()) return ResultErr(std::move(allof_result).UnwrapErr());
    result = SchemaSpec::Make(std::move(allof_result).Unwrap(), cache_key, rule_name_hint);
  } else if (schema_obj.count("type") || default_type.has_value()) {
    if (schema_obj.count("type") && schema_obj.at("type").is<picojson::array>()) {
      auto type_array_result = ParseTypeArray(schema_obj, rule_name_hint);
      if (type_array_result.IsErr()) return ResultErr(std::move(type_array_result).UnwrapErr());
      result = SchemaSpec::Make(std::move(type_array_result).Unwrap(), cache_key, rule_name_hint);
    } else {
      if (schema_obj.count("type") && !schema_obj.at("type").is<std::string>()) {
        return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Type should be a string");
      }
      const std::string& type = schema_obj.count("type") ? schema_obj.at("type").get<std::string>()
                                                         : default_type.value();
      if (type == "integer") {
        auto int_result = ParseInteger(schema_obj);
        if (int_result.IsErr()) return ResultErr(std::move(int_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(int_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "number") {
        auto num_result = ParseNumber(schema_obj);
        if (num_result.IsErr()) return ResultErr(std::move(num_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(num_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "string") {
        auto str_result = ParseString(schema_obj);
        if (str_result.IsErr()) return ResultErr(std::move(str_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(str_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "boolean") {
        auto bool_result = ParseBoolean(schema_obj);
        if (bool_result.IsErr()) return ResultErr(std::move(bool_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(bool_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "null") {
        auto null_result = ParseNull(schema_obj);
        if (null_result.IsErr()) return ResultErr(std::move(null_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(null_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "array") {
        auto array_result = ParseArray(schema_obj);
        if (array_result.IsErr()) return ResultErr(std::move(array_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(array_result).Unwrap(), cache_key, rule_name_hint);
      } else if (type == "object") {
        auto obj_result = ParseObject(schema_obj);
        if (obj_result.IsErr()) return ResultErr(std::move(obj_result).UnwrapErr());
        result = SchemaSpec::Make(std::move(obj_result).Unwrap(), cache_key, rule_name_hint);
      } else {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "Unsupported type \"" + type + "\""
        );
      }
    }
  } else {
    // Type-specific JSON Schema keywords only apply when the instance has that type. Without an
    // explicit `type`, the schema is therefore a union of a constrained branch for each
    // applicable type and unconstrained branches for every other JSON type. Inferring one type
    // from `properties` or `items` incorrectly rejects all non-object/non-array instances.
    static const std::unordered_set<std::string> kNumberKeywords = {
        "multipleOf", "maximum", "exclusiveMaximum", "minimum", "exclusiveMinimum"
    };
    static const std::unordered_set<std::string> kStringKeywords = {
        "maxLength", "minLength", "pattern", "format", "contentEncoding", "contentMediaType"
    };
    static const std::unordered_set<std::string> kArrayKeywords = {
        "maxItems",
        "minItems",
        "uniqueItems",
        "maxContains",
        "minContains",
        "contains",
        "items",
        "additionalItems",
        "prefixItems",
        "unevaluatedItems"
    };
    static const std::unordered_set<std::string> kObjectKeywords = {
        "maxProperties",
        "minProperties",
        "required",
        "additionalProperties",
        "properties",
        "patternProperties",
        "dependencies",
        "dependentRequired",
        "dependentSchemas",
        "propertyNames",
        "unevaluatedProperties"
    };
    auto has_keyword = [&schema_obj](const std::unordered_set<std::string>& keywords) {
      return std::any_of(schema_obj.begin(), schema_obj.end(), [&](const auto& item) {
        return keywords.count(item.first) != 0;
      });
    };
    bool has_number_keywords = has_keyword(kNumberKeywords);
    bool has_string_keywords = has_keyword(kStringKeywords);
    bool has_array_keywords = has_keyword(kArrayKeywords);
    bool has_object_keywords = has_keyword(kObjectKeywords);

    if (!has_number_keywords && !has_string_keywords && !has_array_keywords &&
        !has_object_keywords) {
      result = SchemaSpec::Make(AnySpec{}, cache_key, rule_name_hint);
    } else {
      TypeArraySpec type_union;
      auto parse_constrained_type = [&](const char* type_name
                                    ) -> Result<SchemaSpecPtr, SchemaError> {
        picojson::object typed_schema = schema_obj;
        typed_schema["type"] = picojson::value(type_name);
        auto typed_result =
            Parse(picojson::value(std::move(typed_schema)), rule_name_hint + "_" + type_name);
        if (typed_result.IsErr()) {
          return ResultErr(std::move(typed_result).UnwrapErr());
        }
        return ResultOk(std::move(typed_result).Unwrap());
      };
      auto add_basic_type = [&](auto spec, const char* type_name) {
        type_union.type_schemas.push_back(SchemaSpec::Make(
            std::move(spec),
            std::string("{\"type\":\"") + type_name + "\"}",
            rule_name_hint + "_" + type_name
        ));
      };

      if (has_number_keywords) {
        auto number_result = parse_constrained_type("number");
        if (number_result.IsErr()) return ResultErr(std::move(number_result).UnwrapErr());
        type_union.type_schemas.push_back(std::move(number_result).Unwrap());
      } else {
        add_basic_type(NumberSpec{}, "number");
      }
      if (has_string_keywords) {
        auto string_result = parse_constrained_type("string");
        if (string_result.IsErr()) return ResultErr(std::move(string_result).UnwrapErr());
        type_union.type_schemas.push_back(std::move(string_result).Unwrap());
      } else {
        add_basic_type(StringSpec{}, "string");
      }

      add_basic_type(BooleanSpec{}, "boolean");
      add_basic_type(NullSpec{}, "null");

      if (has_array_keywords) {
        auto array_result = parse_constrained_type("array");
        if (array_result.IsErr()) return ResultErr(std::move(array_result).UnwrapErr());
        type_union.type_schemas.push_back(std::move(array_result).Unwrap());
      } else {
        ArraySpec array_spec;
        array_spec.allow_additional_items = true;
        array_spec.additional_items = SchemaSpec::Make(AnySpec{}, "{}", "any");
        add_basic_type(std::move(array_spec), "array");
      }
      if (has_object_keywords) {
        auto object_result = parse_constrained_type("object");
        if (object_result.IsErr()) return ResultErr(std::move(object_result).UnwrapErr());
        type_union.type_schemas.push_back(std::move(object_result).Unwrap());
      } else {
        ObjectSpec object_spec;
        object_spec.allow_additional_properties = true;
        object_spec.additional_properties_schema = SchemaSpec::Make(AnySpec{}, "{}", "any");
        add_basic_type(std::move(object_spec), "object");
      }
      result = SchemaSpec::Make(std::move(type_union), cache_key, rule_name_hint);
    }
  }

  schema_cache_[cache_key] = result;
  return ResultOk(result);
}

Result<IntegerSpec, SchemaError> SchemaParser::ParseInteger(const picojson::object& schema) {
  IntegerSpec spec;

  auto checkAndConvertIntegerBound = [](const picojson::value& value
                                     ) -> Result<std::string, SchemaError> {
    if (auto exact = DecodeExactNumberBound(value)) {
      auto canonical = ParseCanonicalIntegerLexeme(*exact);
      if (!canonical.has_value()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "Integer constraint must be a whole number"
        );
      }
      return ResultOk<std::string>(std::move(*canonical));
    }
    if (auto exact = DecodeExactIntegerBound(value)) {
      return ResultOk<std::string>(std::move(*exact));
    }
    if (!value.is<int64_t>() && !value.is<double>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Value must be a number");
    }
    if (value.is<int64_t>()) return ResultOk<std::string>(std::to_string(value.get<int64_t>()));
    double val = value.get<double>();
    if (!std::isfinite(val)) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsupportedSchema, "Integer constraint magnitude is too large"
      );
    }
    if (val != std::floor(val)) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "Integer constraint must be a whole number"
      );
    }
    std::ostringstream text;
    text << std::fixed << std::setprecision(0) << val;
    return ResultOk<std::string>(text.str());
  };

  auto checkAndConvertMultipleOf = [](const picojson::value& value
                                   ) -> Result<int64_t, SchemaError> {
    if (auto exact = DecodeExactNumberMultipleOf(value)) {
      int64_t integer_value = exact->first;
      if (exact->second > 0) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kUnsupportedSchema, "multipleOf for type:integer must be an integer"
        );
      }
      for (int32_t zero = 0; zero < -static_cast<int64_t>(exact->second); ++zero) {
        if (integer_value > kIntegerMultipleOfMax / 10) {
          return ResultErr<SchemaError>(
              SchemaErrorType::kUnsupportedSchema,
              "multipleOf for type:integer must be > 0 and <= " +
                  std::to_string(kIntegerMultipleOfMax)
          );
        }
        integer_value *= 10;
      }
      if (integer_value <= 0 || integer_value > kIntegerMultipleOfMax) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kUnsupportedSchema,
            "multipleOf for type:integer must be > 0 and <= " +
                std::to_string(kIntegerMultipleOfMax)
        );
      }
      return ResultOk<int64_t>(integer_value);
    }
    double val;
    if (value.is<int64_t>()) {
      val = static_cast<double>(value.get<int64_t>());
    } else if (value.is<double>()) {
      val = value.get<double>();
    } else {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Value must be a number");
    }
    if (val <= 0) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "multipleOf must be greater than 0"
      );
    }
    if (val != std::floor(val)) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsupportedSchema, "multipleOf for type:integer must be an integer"
      );
    }
    if (val > static_cast<double>(kIntegerMultipleOfMax)) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsupportedSchema,
          "multipleOf for type:integer must be > 0 and <= " + std::to_string(kIntegerMultipleOfMax)
      );
    }
    return ResultOk<int64_t>(static_cast<int64_t>(val));
  };

  if (schema.count("multipleOf")) {
    auto result = checkAndConvertMultipleOf(schema.at("multipleOf"));
    if (result.IsErr()) {
      if (result.ErrRef().Type() != SchemaErrorType::kUnsupportedSchema) {
        return ResultErr(std::move(result).UnwrapErr());
      }
      XGRAMMAR_LOG(WARNING) << result.ErrRef().what() << "; ignoring multipleOf";
    } else {
      spec.multiple_of = std::move(result).Unwrap();
    }
  }
  if (schema.count("minimum")) {
    auto result = checkAndConvertIntegerBound(schema.at("minimum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.minimum = std::move(result).Unwrap();
  }
  if (schema.count("maximum")) {
    auto result = checkAndConvertIntegerBound(schema.at("maximum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.maximum = std::move(result).Unwrap();
  }
  if (schema.count("exclusiveMinimum")) {
    auto result = checkAndConvertIntegerBound(schema.at("exclusiveMinimum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.exclusive_minimum = std::move(result).Unwrap();
  }
  if (schema.count("exclusiveMaximum")) {
    auto result = checkAndConvertIntegerBound(schema.at("exclusiveMaximum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.exclusive_maximum = std::move(result).Unwrap();
  }

  EffectiveIntegerRange effective_range = ComputeEffectiveIntegerRange(spec);
  if (effective_range.start.has_value() && effective_range.end.has_value() &&
      CompareCanonicalIntegers(*effective_range.start, *effective_range.end) > 0) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "Invalid range: minimum greater than maximum"
    );
  }
  if (spec.multiple_of.has_value()) {
    auto effective_min = effective_range.start.has_value()
                             ? TryConvertToInt64(*effective_range.start)
                             : std::nullopt;
    auto effective_max =
        effective_range.end.has_value() ? TryConvertToInt64(*effective_range.end) : std::nullopt;
    // A small finite range is enumerated during emission, so diagnose an empty intersection here.
    // Half-bounded, wide, and beyond-int64 ranges retain multipleOf and use the exact streaming
    // remainder constraint instead.
    if (effective_min.has_value() && effective_max.has_value() &&
        !IsRangeWidthOverCap(*effective_min, *effective_max, kIntegerMultipleOfRangeWidthMax)) {
      if (!HasMultipleInRange(*effective_min, *effective_max, *spec.multiple_of)) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kUnsatisfiableSchema, "range contains no multipleOf value"
        );
      }
    }
  }
  return ResultOk(std::move(spec));
}

Result<NumberSpec, SchemaError> SchemaParser::ParseNumber(const picojson::object& schema) {
  NumberSpec spec;
  if (schema.count("multipleOf")) {
    const auto& value = schema.at("multipleOf");
    auto exact = DecodeExactNumberMultipleOf(value);
    if (!exact.has_value() && !value.is<int64_t>() && !value.is<double>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Value must be a number");
    }
    if (!exact.has_value()) {
      const double numeric_value =
          value.is<int64_t>() ? static_cast<double>(value.get<int64_t>()) : value.get<double>();
      if (!(numeric_value > 0)) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "multipleOf must be greater than 0"
        );
      }
    }
    if (!exact.has_value()) {
      exact = ParseExactNumberMultipleOfLexeme(value.serialize());
    }
    if (!exact.has_value()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsupportedSchema,
          "multipleOf must have at most " +
              std::to_string(std::to_string(kNumberMultipleOfCoefficientMax).size()) +
              " significant decimal digits"
      );
    }
    spec.multiple_of = *exact;
  }

  auto getExact = [](const picojson::value& value) -> Result<std::string, SchemaError> {
    if (auto exact = DecodeExactNumberBound(value)) {
      if (ParseExactDecimalLexeme(*exact).has_value()) {
        return ResultOk<std::string>(std::move(*exact));
      }
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Value must be a number");
    }
    if (auto exact = DecodeExactIntegerBound(value)) {
      if (ParseExactDecimalLexeme(*exact).has_value()) {
        return ResultOk<std::string>(std::move(*exact));
      }
    }
    if (!value.is<double>() && !value.is<int64_t>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Value must be a number");
    }
    std::string lexeme = value.serialize();
    if (!ParseExactDecimalLexeme(lexeme).has_value()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "Value must be a number");
    }
    return ResultOk<std::string>(std::move(lexeme));
  };

  if (schema.count("minimum")) {
    auto result = getExact(schema.at("minimum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.minimum = std::move(result).Unwrap();
  }
  if (schema.count("maximum")) {
    auto result = getExact(schema.at("maximum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.maximum = std::move(result).Unwrap();
  }
  if (schema.count("exclusiveMinimum")) {
    auto result = getExact(schema.at("exclusiveMinimum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.exclusive_minimum = std::move(result).Unwrap();
  }
  if (schema.count("exclusiveMaximum")) {
    auto result = getExact(schema.at("exclusiveMaximum"));
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    spec.exclusive_maximum = std::move(result).Unwrap();
  }

  // The range is empty if any lower bound conflicts with any upper bound. An
  // exclusive bound also rules out equality, so it uses ">=" instead of ">".
  auto empty = []() {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema, "Invalid range: empty range"
    );
  };

  // minimum (x >= min) vs maximum (x <= max).
  if (spec.minimum && spec.maximum &&
      CompareExactDecimalLexemes(*spec.minimum, *spec.maximum) > 0) {
    return empty();
  }
  // minimum (x >= min) vs exclusiveMaximum (x < exclMax).
  if (spec.minimum && spec.exclusive_maximum &&
      CompareExactDecimalLexemes(*spec.minimum, *spec.exclusive_maximum) >= 0) {
    return empty();
  }
  // exclusiveMinimum (x > exclMin) vs maximum (x <= max).
  if (spec.exclusive_minimum && spec.maximum &&
      CompareExactDecimalLexemes(*spec.exclusive_minimum, *spec.maximum) >= 0) {
    return empty();
  }
  // exclusiveMinimum (x > exclMin) vs exclusiveMaximum (x < exclMax).
  if (spec.exclusive_minimum && spec.exclusive_maximum &&
      CompareExactDecimalLexemes(*spec.exclusive_minimum, *spec.exclusive_maximum) >= 0) {
    return empty();
  }
  return ResultOk(std::move(spec));
}

Result<StringSpec, SchemaError> SchemaParser::ParseString(const picojson::object& schema) {
  StringSpec spec;
  if (schema.count("format")) spec.format = schema.at("format").get<std::string>();
  if (schema.count("pattern")) spec.pattern = schema.at("pattern").get<std::string>();
  if (schema.count("minLength")) {
    if (!schema.at("minLength").is<int64_t>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "minLength must be an integer"
      );
    }
    spec.min_length = static_cast<int>(schema.at("minLength").get<int64_t>());
  }
  if (schema.count("maxLength")) {
    if (!schema.at("maxLength").is<int64_t>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "maxLength must be an integer"
      );
    }
    spec.max_length = static_cast<int>(schema.at("maxLength").get<int64_t>());
  }
  if (spec.max_length != -1 && spec.min_length > spec.max_length) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "minLength " + std::to_string(spec.min_length) + " is greater than maxLength " +
            std::to_string(spec.max_length)
    );
  }
  return ResultOk(std::move(spec));
}

Result<BooleanSpec, SchemaError> SchemaParser::ParseBoolean(const picojson::object&) {
  return ResultOk(BooleanSpec{});
}

Result<NullSpec, SchemaError> SchemaParser::ParseNull(const picojson::object&) {
  return ResultOk(NullSpec{});
}

Result<ArraySpec, SchemaError> SchemaParser::ParseArray(const picojson::object& schema) {
  WarnUnsupportedKeywords(schema, {"uniqueItems", "contains", "minContains", "maxContains"});
  ArraySpec spec;

  if (schema.count("prefixItems")) {
    if (!schema.at("prefixItems").is<picojson::array>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "prefixItems must be an array"
      );
    }
    for (const auto& item : schema.at("prefixItems").get<picojson::array>()) {
      if (!item.is<bool>() && !item.is<picojson::object>()) {
        return ResultErr<SchemaError>(
            SchemaErrorType::kInvalidSchema, "prefixItems must be an array of objects or booleans"
        );
      }
      auto item_result = Parse(item, "prefix_item");
      if (item_result.IsErr()) return ResultErr(std::move(item_result).UnwrapErr());
      spec.prefix_items.push_back(std::move(item_result).Unwrap());
    }
  }

  if (schema.count("items")) {
    auto items_value = schema.at("items");
    if (!items_value.is<bool>() && !items_value.is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "items must be a boolean or an object"
      );
    }
    if (items_value.is<bool>() && !items_value.get<bool>()) {
      spec.allow_additional_items = false;
    } else {
      spec.allow_additional_items = true;
      auto items_result = Parse(items_value, "item");
      if (items_result.IsErr()) return ResultErr(std::move(items_result).UnwrapErr());
      spec.additional_items = std::move(items_result).Unwrap();
    }
  } else if (schema.count("unevaluatedItems")) {
    auto unevaluated_items_value = schema.at("unevaluatedItems");
    if (!unevaluated_items_value.is<bool>() && !unevaluated_items_value.is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "unevaluatedItems must be a boolean or an object"
      );
    }
    if (unevaluated_items_value.is<bool>() && !unevaluated_items_value.get<bool>()) {
      spec.allow_additional_items = false;
    } else {
      spec.allow_additional_items = true;
      auto items_result = Parse(unevaluated_items_value, "unevaluated_item");
      if (items_result.IsErr()) return ResultErr(std::move(items_result).UnwrapErr());
      spec.additional_items = std::move(items_result).Unwrap();
    }
  } else if (!config_.strict_mode) {
    spec.allow_additional_items = true;
    spec.additional_items = SchemaSpec::Make(AnySpec{}, "", "any");
  } else {
    spec.allow_additional_items = false;
  }

  if (schema.count("minItems")) {
    if (!schema.at("minItems").is<int64_t>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "minItems must be an integer");
    }
    spec.min_items = std::max(static_cast<int64_t>(0), schema.at("minItems").get<int64_t>());
  }
  if (schema.count("minContains")) {
    if (!schema.at("minContains").is<int64_t>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "minContains must be an integer"
      );
    }
    spec.min_items = std::max(spec.min_items, schema.at("minContains").get<int64_t>());
  }
  if (schema.count("maxItems")) {
    if (!schema.at("maxItems").is<int64_t>() || schema.at("maxItems").get<int64_t>() < 0) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "maxItems must be a non-negative integer"
      );
    }
    spec.max_items = schema.at("maxItems").get<int64_t>();
  }

  if (spec.max_items != -1 && spec.min_items > spec.max_items) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "minItems is greater than maxItems: " + std::to_string(spec.min_items) + " > " +
            std::to_string(spec.max_items)
    );
  }
  if (!spec.allow_additional_items) {
    int64_t prefix_size = static_cast<int64_t>(spec.prefix_items.size());
    if (prefix_size < spec.min_items) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema,
          "minItems is greater than the number of prefixItems, but additional items are not "
          "allowed: " +
              std::to_string(spec.min_items) + " > " + std::to_string(prefix_size)
      );
    }
    if (spec.max_items != -1 && prefix_size > spec.max_items) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema,
          "maxItems is less than the number of prefixItems, but additional items are not "
          "allowed: " +
              std::to_string(spec.max_items) + " < " + std::to_string(prefix_size)
      );
    }
  }
  return ResultOk(std::move(spec));
}

bool IsSchemaAnnotationKeyword(const std::string& keyword) {
  static const std::unordered_set<std::string> kAnnotations = {
      "$comment",
      "$id",
      "$schema",
      "$defs",
      "definitions",
      "default",
      "deprecated",
      "description",
      "examples",
      "readOnly",
      "title",
      "writeOnly",
  };
  return kAnnotations.count(keyword) != 0;
}

std::optional<std::unordered_set<std::string>> GetExplicitSchemaTypes(const picojson::value& schema
) {
  if (!schema.is<picojson::object>()) {
    return std::nullopt;
  }
  const auto& object = schema.get<picojson::object>();
  if (!object.count("type")) {
    return std::nullopt;
  }
  std::unordered_set<std::string> result;
  const auto& type = object.at("type");
  if (type.is<std::string>()) {
    result.insert(type.get<std::string>());
    return result;
  }
  if (!type.is<picojson::array>()) {
    return std::nullopt;
  }
  for (const auto& option : type.get<picojson::array>()) {
    if (!option.is<std::string>()) {
      return std::nullopt;
    }
    result.insert(option.get<std::string>());
  }
  return result;
}

bool KeywordAppliesToAnyType(
    const std::string& keyword, const std::unordered_set<std::string>& explicit_types
) {
  static const std::unordered_set<std::string> kNumberKeywords = {
      "exclusiveMaximum", "exclusiveMinimum", "maximum", "minimum", "multipleOf"
  };
  static const std::unordered_set<std::string> kStringKeywords = {
      "contentEncoding", "contentMediaType", "format", "maxLength", "minLength", "pattern"
  };
  static const std::unordered_set<std::string> kArrayKeywords = {
      "additionalItems",
      "contains",
      "items",
      "maxContains",
      "maxItems",
      "minContains",
      "minItems",
      "prefixItems",
      "unevaluatedItems",
      "uniqueItems",
  };
  static const std::unordered_set<std::string> kObjectKeywords = {
      "additionalProperties",
      "dependencies",
      "dependentRequired",
      "dependentSchemas",
      "maxProperties",
      "minProperties",
      "patternProperties",
      "properties",
      "propertyNames",
      "required",
      "unevaluatedProperties",
  };
  auto contains_type = [&](const char* type) { return explicit_types.count(type) != 0; };
  if (kNumberKeywords.count(keyword)) {
    return contains_type("number") || contains_type("integer");
  }
  if (kStringKeywords.count(keyword)) {
    return contains_type("string");
  }
  if (kArrayKeywords.count(keyword)) {
    return contains_type("array");
  }
  if (kObjectKeywords.count(keyword)) {
    return contains_type("object");
  }
  // Unknown and type-independent assertion keywords are conservatively relevant.
  return true;
}

/*! \brief A narrow, proof-producing implication check for an overlapping named property.
 *
 * Returning true is sufficient to emit the named key only through its `properties` branch. The
 * proof is deliberately syntactic: the pattern schema is true/annotation-only, every assertion is
 * already present verbatim in the named schema, or its type-specific keywords cannot apply to any
 * explicit type allowed by the named schema. Unsupported cases remain on the conservative path.
 */
bool NamedPropertyImpliesPatternProperty(
    const picojson::value& named_schema, const picojson::value& pattern_schema
) {
  if (named_schema.is<bool>() && !named_schema.get<bool>()) {
    return true;
  }
  if (pattern_schema.is<bool>()) {
    return pattern_schema.get<bool>() || (named_schema.is<bool>() && !named_schema.get<bool>());
  }
  if (named_schema.serialize(false) == pattern_schema.serialize(false)) {
    return true;
  }
  if (!named_schema.is<picojson::object>() || !pattern_schema.is<picojson::object>()) {
    return false;
  }

  const auto& named_object = named_schema.get<picojson::object>();
  const auto& pattern_object = pattern_schema.get<picojson::object>();
  bool all_assertions_are_present = true;
  bool has_assertion = false;
  for (const auto& [keyword, value] : pattern_object) {
    if (IsSchemaAnnotationKeyword(keyword)) {
      continue;
    }
    has_assertion = true;
    auto named_it = named_object.find(keyword);
    if (named_it == named_object.end() ||
        named_it->second.serialize(false) != value.serialize(false)) {
      all_assertions_are_present = false;
      break;
    }
  }
  if (!has_assertion || all_assertions_are_present) {
    return true;
  }

  auto explicit_types = GetExplicitSchemaTypes(named_schema);
  if (!explicit_types.has_value()) {
    return false;
  }
  for (const auto& [keyword, value] : pattern_object) {
    static_cast<void>(value);
    if (!IsSchemaAnnotationKeyword(keyword) && KeywordAppliesToAnyType(keyword, *explicit_types)) {
      return false;
    }
  }
  return true;
}

Result<ObjectSpec, SchemaError> SchemaParser::ParseObject(const picojson::object& schema) {
  ObjectSpec spec;

  std::unordered_map<std::string, std::vector<std::string>> pattern_excluded_properties;
  std::unordered_map<std::string, FSMWithStartEnd> pattern_fsms;
  std::unordered_set<std::string> failed_pattern_fsms;
  const picojson::object* pattern_properties = nullptr;
  if (schema.count("patternProperties") && schema.at("patternProperties").is<picojson::object>()) {
    pattern_properties = &schema.at("patternProperties").get<picojson::object>();
  }
  auto pattern_matches_property = [&](const std::string& pattern,
                                      const std::string& property) -> std::optional<bool> {
    auto cached = pattern_fsms.find(pattern);
    if (cached == pattern_fsms.end() && !failed_pattern_fsms.count(pattern)) {
      auto built = BuildJSONSchemaPatternFSM(pattern, kJSONSchemaPatternDFAStateLimit);
      if (built.IsOk()) {
        cached = pattern_fsms.emplace(pattern, std::move(built).Unwrap()).first;
      } else {
        failed_pattern_fsms.insert(pattern);
      }
    }
    if (cached == pattern_fsms.end()) {
      return std::nullopt;
    }
    return cached->second.AcceptString(property);
  };

  if (schema.count("properties")) {
    if (!schema.at("properties").is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "properties must be an object"
      );
    }
    auto properties_obj = schema.at("properties").get<picojson::object>();
    for (const auto& key : properties_obj.ordered_keys()) {
      const picojson::value& declared_schema = properties_obj.at(key);
      picojson::value effective_schema = declared_schema;
      std::vector<std::string> matching_patterns;
      if (pattern_properties != nullptr) {
        for (const auto& pattern : pattern_properties->ordered_keys()) {
          if (pattern_matches_property(pattern, key) == true) matching_patterns.push_back(pattern);
        }
      }

      // A named property that also matches patternProperties must satisfy both schemas. Prefer an
      // exact typed/object allOf merge; when it succeeds the named branch owns the key and the
      // regex alternatives can exclude it unconditionally. If the conjunction is beyond the
      // current exact merger, retain the previous proof-based exclusions without weakening the
      // declared property schema.
      bool exactly_conjoined_patterns = false;
      if (!matching_patterns.empty()) {
        picojson::array conjuncts;
        conjuncts.push_back(declared_schema);
        for (const auto& pattern : matching_patterns) {
          conjuncts.push_back(pattern_properties->at(pattern));
        }
        picojson::object all_of_schema;
        all_of_schema["allOf"] = picojson::value(std::move(conjuncts));
        if (auto merged = TryMergeObjectAllOf(all_of_schema)) {
          effective_schema = std::move(*merged);
          exactly_conjoined_patterns = true;
        } else if (auto merged = TryMergeTypedAllOf(all_of_schema)) {
          effective_schema = std::move(*merged);
          exactly_conjoined_patterns = true;
        }
      }

      auto prop_result = Parse(effective_schema, key);
      if (prop_result.IsErr()) return ResultErr(std::move(prop_result).UnwrapErr());
      spec.properties.push_back({key, std::move(prop_result).Unwrap()});
      for (const auto& pattern : matching_patterns) {
        if (exactly_conjoined_patterns ||
            NamedPropertyImpliesPatternProperty(declared_schema, pattern_properties->at(pattern))) {
          pattern_excluded_properties[pattern].push_back(key);
        }
      }
    }
  }

  if (schema.count("required")) {
    if (!schema.at("required").is<picojson::array>()) {
      return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "required must be an array");
    }
    for (const auto& req : schema.at("required").get<picojson::array>()) {
      spec.required.insert(req.get<std::string>());
    }
  }

  if (schema.count("patternProperties")) {
    if (!schema.at("patternProperties").is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "patternProperties must be an object"
      );
    }
    auto pattern_props = schema.at("patternProperties").get<picojson::object>();
    for (const auto& key : pattern_props.ordered_keys()) {
      auto prop_result = Parse(pattern_props.at(key), "pattern_prop");
      if (prop_result.IsErr()) return ResultErr(std::move(prop_result).UnwrapErr());
      spec.pattern_properties.push_back(
          {key, std::move(prop_result).Unwrap(), std::move(pattern_excluded_properties[key])}
      );
    }
  }

  if (schema.count("propertyNames")) {
    const auto& property_names = schema.at("propertyNames");
    if (!property_names.is<bool>() && !property_names.is<picojson::object>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "propertyNames must be an object or boolean"
      );
    }
    if (property_names.is<bool>()) {
      spec.property_names = property_names.get<bool>()
                                ? SchemaSpec::Make(StringSpec{}, "", "property_name")
                                : SchemaSpec::Make(NeverSpec{}, "", "property_name");
    } else {
      const auto& property_names_obj = property_names.get<picojson::object>();
      if (property_names_obj.count("type") && property_names_obj.at("type").is<std::string>() &&
          property_names_obj.at("type").get<std::string>() != "string") {
        return ResultErr<SchemaError>(
            SchemaErrorType::kUnsatisfiableSchema,
            "propertyNames must be an object that validates string"
        );
      }
      auto prop_names_result = Parse(property_names, "property_name", "string");
      if (prop_names_result.IsErr()) return ResultErr(std::move(prop_names_result).UnwrapErr());
      spec.property_names = std::move(prop_names_result).Unwrap();
    }
  }

  spec.allow_additional_properties = !config_.strict_mode;
  if (schema.count("additionalProperties")) {
    auto add_props = schema.at("additionalProperties");
    if (add_props.is<bool>()) {
      spec.allow_additional_properties = add_props.get<bool>();
    } else {
      spec.allow_additional_properties = true;
      auto add_props_result = Parse(add_props, "additional");
      if (add_props_result.IsErr()) return ResultErr(std::move(add_props_result).UnwrapErr());
      spec.additional_properties_schema = std::move(add_props_result).Unwrap();
    }
  }

  spec.allow_unevaluated_properties = true;
  if (schema.count("additionalProperties")) {
    spec.allow_unevaluated_properties = spec.allow_additional_properties;
  } else if (schema.count("unevaluatedProperties")) {
    auto uneval_props = schema.at("unevaluatedProperties");
    if (uneval_props.is<bool>()) {
      spec.allow_unevaluated_properties = uneval_props.get<bool>();
    } else {
      spec.allow_unevaluated_properties = true;
      auto uneval_result = Parse(uneval_props, "unevaluated");
      if (uneval_result.IsErr()) return ResultErr(std::move(uneval_result).UnwrapErr());
      spec.unevaluated_properties_schema = std::move(uneval_result).Unwrap();
    }
  } else if (config_.strict_mode) {
    spec.allow_unevaluated_properties = false;
  }

  if (schema.count("minProperties")) {
    if (!schema.at("minProperties").is<int64_t>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "minProperties must be an integer"
      );
    }
    spec.min_properties = static_cast<int>(schema.at("minProperties").get<int64_t>());
    if (spec.min_properties < 0) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "minProperties must be a non-negative integer"
      );
    }
  }
  if (schema.count("maxProperties")) {
    if (!schema.at("maxProperties").is<int64_t>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "maxProperties must be an integer"
      );
    }
    spec.max_properties = static_cast<int>(schema.at("maxProperties").get<int64_t>());
    if (spec.max_properties < 0) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kUnsatisfiableSchema, "maxProperties must be a non-negative integer"
      );
    }
  }

  if (spec.max_properties != -1 && spec.min_properties > spec.max_properties) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "minProperties is greater than maxProperties: " + std::to_string(spec.min_properties) +
            " > " + std::to_string(spec.max_properties)
    );
  }
  if (spec.max_properties != -1 && static_cast<int>(spec.required.size()) > spec.max_properties) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "maxProperties is less than the number of required properties: " +
            std::to_string(spec.max_properties) + " < " + std::to_string(spec.required.size())
    );
  }

  // `required` names do not have to appear in `properties`. Materialize every undeclared fixed
  // name whose applicable value schema can be determined exactly. This lets the any-order
  // required-key state machine track actual names instead of approximating them with an object-
  // property count. A fixed name matching patternProperties uses the conjunction of every
  // matching value schema and is then excluded from the regex-backed alternatives. With
  // propertyNames present, a named branch would also have to prove that the fixed key itself
  // satisfies that schema, so retain the conservative fallback for that case.
  if (!spec.property_names) {
    std::unordered_set<std::string> named_properties;
    for (const auto& property : spec.properties) named_properties.insert(property.name);
    std::vector<std::string> undeclared_required;
    std::unordered_set<std::string> seen_undeclared_required;
    if (schema.count("required")) {
      for (const auto& required_value : schema.at("required").get<picojson::array>()) {
        const auto& required_name = required_value.get<std::string>();
        if (!named_properties.count(required_name) &&
            seen_undeclared_required.insert(required_name).second) {
          undeclared_required.push_back(required_name);
        }
      }
    }
    for (const auto& required_name : undeclared_required) {
      SchemaSpecPtr value_schema;
      std::vector<std::string> matching_patterns;
      bool all_pattern_matches_known = true;
      if (pattern_properties != nullptr) {
        for (const auto& pattern : pattern_properties->ordered_keys()) {
          auto matches = pattern_matches_property(pattern, required_name);
          if (!matches.has_value()) {
            all_pattern_matches_known = false;
          } else if (*matches) {
            matching_patterns.push_back(pattern);
          }
        }
      }
      if (!all_pattern_matches_known) {
        continue;
      }

      if (!matching_patterns.empty()) {
        std::vector<picojson::value> effective_conjuncts;
        for (const auto& pattern : matching_patterns) {
          const picojson::value& pattern_schema = pattern_properties->at(pattern);
          if (pattern_schema.is<bool>()) {
            if (!pattern_schema.get<bool>()) {
              effective_conjuncts.assign(1, picojson::value(false));
              break;
            }
            continue;
          }
          effective_conjuncts.push_back(pattern_schema);
        }

        picojson::value effective_schema(true);
        if (!effective_conjuncts.empty()) {
          effective_schema = effective_conjuncts.front();
          bool all_identical = std::all_of(
              effective_conjuncts.begin() + 1,
              effective_conjuncts.end(),
              [&](const picojson::value& conjunct) {
                return conjunct.serialize(false) == effective_schema.serialize(false);
              }
          );
          if (effective_conjuncts.size() > 1 && !all_identical) {
            picojson::object all_of_schema;
            all_of_schema["allOf"] = picojson::value(
                picojson::array(effective_conjuncts.begin(), effective_conjuncts.end())
            );
            auto merged_schema = TryMergeObjectAllOf(all_of_schema);
            if (!merged_schema.has_value()) {
              merged_schema = TryMergeTypedAllOf(all_of_schema);
            }
            if (!merged_schema.has_value()) {
              continue;
            }
            effective_schema = std::move(*merged_schema);
          }
        }

        auto required_result = Parse(effective_schema, required_name);
        if (required_result.IsErr()) {
          return ResultErr(std::move(required_result).UnwrapErr());
        }
        value_schema = std::move(required_result).Unwrap();
        for (auto& pattern_property : spec.pattern_properties) {
          if (std::find(
                  matching_patterns.begin(), matching_patterns.end(), pattern_property.pattern
              ) != matching_patterns.end()) {
            pattern_property.excluded_property_names.push_back(required_name);
          }
        }
      } else if (spec.allow_additional_properties) {
        value_schema = spec.additional_properties_schema;
      } else if (spec.allow_unevaluated_properties) {
        value_schema = spec.unevaluated_properties_schema;
      } else {
        // This is a valid but unsatisfiable object schema, not an invalid schema. Materialize the
        // required key with a never-matching value schema so the object itself becomes impossible
        // while its parent remains usable. This matters for schemas such as an array whose `items`
        // schema is unsatisfiable: the array must still accept instances with no such items.
        value_schema = SchemaSpec::Make(NeverSpec{}, "false", "required_forbidden");
      }
      if (value_schema == nullptr) {
        value_schema = SchemaSpec::Make(AnySpec{}, "{}", "required_additional");
      }
      spec.properties.push_back({required_name, std::move(value_schema)});
      named_properties.insert(required_name);
    }
  }

  if (spec.pattern_properties.empty() && !spec.property_names &&
      !spec.allow_additional_properties && !spec.allow_unevaluated_properties &&
      spec.min_properties > static_cast<int>(spec.properties.size())) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kUnsatisfiableSchema,
        "minProperties is greater than the number of properties, but additional properties aren't "
        "allowed: " +
            std::to_string(spec.min_properties) + " > " + std::to_string(spec.properties.size())
    );
  }
  return ResultOk(std::move(spec));
}

Result<ConstSpec, SchemaError> SchemaParser::ParseConst(const picojson::object& schema) {
  ConstSpec spec;
  spec.json_value = schema.at("const").serialize();
  return ResultOk(std::move(spec));
}

Result<EnumSpec, SchemaError> SchemaParser::ParseEnum(const picojson::object& schema) {
  EnumSpec spec;
  if (!schema.at("enum").is<picojson::array>()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "enum must be an array");
  }
  const auto& enum_array = schema.at("enum").get<picojson::array>();
  if (enum_array.empty()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "enum array must not be empty");
  }
  spec.json_values.reserve(enum_array.size());
  for (const auto& value : enum_array) {
    spec.json_values.push_back(value.serialize());
  }
  return ResultOk(std::move(spec));
}

Result<RefSpec, SchemaError> SchemaParser::ParseRef(const picojson::object& schema) {
  if (!schema.at("$ref").is<std::string>()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "$ref must be a string");
  }
  RefSpec spec;
  spec.uri = schema.at("$ref").get<std::string>();
  return ResultOk(std::move(spec));
}

picojson::value SchemaParser::ConjoinWithSiblingAssertions(
    const picojson::value& option, const picojson::object& sibling_assertions
) const {
  XGRAMMAR_DCHECK(!sibling_assertions.empty());

  // Follow the same replacement semantics as Parse(): a local $ref is authoritative and its
  // siblings are ignored. Looking through the reference here is only used to expose a nested
  // combinator; terminal options retain their original reference and normal cache behavior.
  const picojson::value* resolved = &option;
  std::unordered_set<std::string> active_refs;
  for (int depth = 0; depth <= 64 && resolved->is<picojson::object>(); ++depth) {
    const auto& object = resolved->get<picojson::object>();
    auto ref_it = object.find("$ref");
    if (ref_it == object.end() || !ref_it->second.is<std::string>()) break;
    const std::string& uri = ref_it->second.get<std::string>();
    const picojson::value* target = ResolveLocalJSONPointer(uri);
    if (target == nullptr || !active_refs.insert(uri).second) break;
    resolved = target;
  }

  picojson::object remaining_sibling_assertions = sibling_assertions;
  if (resolved->is<picojson::object>()) {
    const auto& object = resolved->get<picojson::object>();
    for (auto it = remaining_sibling_assertions.begin();
         it != remaining_sibling_assertions.end();) {
      auto option_it = object.find(it->first);
      if (option_it != object.end() &&
          option_it->second.serialize(false) == it->second.serialize(false)) {
        it = remaining_sibling_assertions.erase(it);
      } else {
        ++it;
      }
    }
    // Avoid manufacturing an allOf for an assertion the option already enforces. In strict mode,
    // a synthetic duplicate {"type":"object"} conjunct would otherwise be narrowed to an empty
    // closed object and make exact object merging fail.
    if (remaining_sibling_assertions.empty()) {
      return option;
    }
  }

  if (resolved->is<picojson::object>()) {
    const auto& object = resolved->get<picojson::object>();
    // Match Parse()'s keyword precedence. const/enum remain terminal even if a lower-precedence
    // combinator is also present.
    const char* nested_keyword = nullptr;
    if (!object.count("const") && !object.count("enum")) {
      if (object.count("anyOf")) {
        nested_keyword = "anyOf";
      } else if (object.count("oneOf")) {
        nested_keyword = "oneOf";
      }
    }
    if (nested_keyword != nullptr && object.at(nested_keyword).is<picojson::array>()) {
      picojson::object distributed;
      distributed[nested_keyword] = object.at(nested_keyword);

      picojson::object nested_sibling_assertions;
      for (const auto& [key, value] : object) {
        if (key != nested_keyword && !IsSchemaAnnotationKey(key) &&
            IsKnownSchemaAssertionKeyword(key)) {
          nested_sibling_assertions[key] = value;
        }
      }

      picojson::array common_assertions;
      if (!nested_sibling_assertions.empty()) {
        common_assertions.emplace_back(std::move(nested_sibling_assertions));
      }
      common_assertions.emplace_back(remaining_sibling_assertions);
      distributed["allOf"] = picojson::value(std::move(common_assertions));
      return picojson::value(std::move(distributed));
    }
  }

  picojson::array conjunction{option, picojson::value(std::move(remaining_sibling_assertions))};
  picojson::object wrapped;
  wrapped["allOf"] = picojson::value(std::move(conjunction));
  return picojson::value(std::move(wrapped));
}

Result<SchemaSpecPtr, SchemaError> SchemaParser::ResolveRef(
    const std::string& uri, const std::string& rule_name_hint
) {
  if (ref_cache_.count(uri)) return ResultOk(ref_cache_[uri]);

  if (uri == "#") {
    auto placeholder = SchemaSpec::Make(AnySpec{}, "", "root");
    ref_cache_[uri] = placeholder;
    auto result = Parse(root_schema_, "root");
    if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
    auto resolved = std::move(result).Unwrap();
    ref_cache_[uri] = resolved;
    return ResultOk(resolved);
  }

  if (uri.empty() || uri[0] != '#') {
    XGRAMMAR_LOG(WARNING) << "URI should either be '#' or start with '#/' but got " << uri;
    return ResultOk(SchemaSpec::Make(AnySpec{}, "", "any"));
  }

  const picojson::value* target = ResolveLocalJSONPointer(uri);
  if (target == nullptr) {
    return ResultErr<SchemaError>(
        SchemaErrorType::kInvalidSchema, "Cannot resolve local reference " + uri
    );
  }

  std::string new_rule_name_prefix;
  for (char c : uri) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
      new_rule_name_prefix += c;
    }
  }
  if (new_rule_name_prefix.empty()) {
    new_rule_name_prefix = rule_name_hint;
  }

  auto result = Parse(*target, new_rule_name_prefix);
  if (result.IsErr()) return ResultErr(std::move(result).UnwrapErr());
  auto resolved = std::move(result).Unwrap();
  ref_cache_[uri] = resolved;
  return ResultOk(resolved);
}

Result<AnyOfSpec, SchemaError> SchemaParser::ParseAnyOf(
    const picojson::object& schema, const std::string& keyword
) {
  AnyOfSpec spec;
  if (!schema.at(keyword).is<picojson::array>()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, keyword + " must be an array");
  }
  if (schema.at(keyword).get<picojson::array>().empty()) {
    spec.options.push_back(SchemaSpec::Make(NeverSpec{}, "false", "case_0"));
    return ResultOk(std::move(spec));
  }

  picojson::object sibling_assertions;
  for (const auto& [key, value] : schema) {
    if (key != keyword && !IsSchemaAnnotationKey(key) && IsKnownSchemaAssertionKeyword(key)) {
      sibling_assertions[key] = value;
    }
  }
  int idx = 0;
  for (const auto& option : schema.at(keyword).get<picojson::array>()) {
    picojson::value effective_option = option;
    if (!sibling_assertions.empty()) {
      effective_option = ConjoinWithSiblingAssertions(option, sibling_assertions);
    }
    auto option_result = Parse(effective_option, "case_" + std::to_string(idx));
    if (option_result.IsErr()) return ResultErr(std::move(option_result).UnwrapErr());
    spec.options.push_back(std::move(option_result).Unwrap());
    ++idx;
  }
  return ResultOk(std::move(spec));
}

Result<OneOfSpec, SchemaError> SchemaParser::ParseOneOf(const picojson::object& schema) {
  OneOfSpec spec;
  if (!schema.at("oneOf").is<picojson::array>()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "oneOf must be an array");
  }

  const auto& options = schema.at("oneOf").get<picojson::array>();
  if (options.empty()) {
    spec.options.push_back(SchemaSpec::Make(NeverSpec{}, "false", "case_0"));
    return ResultOk(std::move(spec));
  }

  if (auto rewritten = RewriteExclusiveRequiredPropertyOneOf(schema)) {
    int idx = 0;
    for (const auto& option : *rewritten) {
      auto option_result = Parse(option, "case_" + std::to_string(idx));
      if (option_result.IsErr()) return ResultErr(std::move(option_result).UnwrapErr());
      spec.options.push_back(std::move(option_result).Unwrap());
      ++idx;
    }
    return ResultOk(std::move(spec));
  }

  picojson::object sibling_assertions;
  for (const auto& [key, value] : schema) {
    if (key != "oneOf" && !IsSchemaAnnotationKey(key) && IsKnownSchemaAssertionKeyword(key)) {
      sibling_assertions[key] = value;
    }
  }
  int idx = 0;
  for (const auto& option : options) {
    picojson::value effective_option = option;
    if (!sibling_assertions.empty()) {
      effective_option = ConjoinWithSiblingAssertions(option, sibling_assertions);
    }
    auto option_result = Parse(effective_option, "case_" + std::to_string(idx));
    if (option_result.IsErr()) return ResultErr(std::move(option_result).UnwrapErr());
    spec.options.push_back(std::move(option_result).Unwrap());
    ++idx;
  }

  if (!TryProvePairwiseDisjointOneOf(options)) {
    return ResultErr<SchemaError>(SchemaErrorType::kUnsupportedSchema, kUnsupportedOneOfMessage);
  }

  return ResultOk(std::move(spec));
}

Result<AllOfSpec, SchemaError> SchemaParser::ParseAllOf(const picojson::object& schema) {
  AllOfSpec spec;
  if (!schema.at("allOf").is<picojson::array>()) {
    return ResultErr<SchemaError>(SchemaErrorType::kInvalidSchema, "allOf must be an array");
  }
  int idx = 0;
  for (const auto& sub_schema : schema.at("allOf").get<picojson::array>()) {
    auto sub_result = Parse(sub_schema, "all_" + std::to_string(idx));
    if (sub_result.IsErr()) return ResultErr(std::move(sub_result).UnwrapErr());
    spec.schemas.push_back(std::move(sub_result).Unwrap());
    ++idx;
  }
  return ResultOk(std::move(spec));
}

Result<TypeArraySpec, SchemaError> SchemaParser::ParseTypeArray(
    const picojson::object& schema, const std::string& rule_name_hint
) {
  TypeArraySpec spec;
  auto type_array = schema.at("type").get<picojson::array>();
  if (type_array.empty()) {
    spec.type_schemas.push_back(SchemaSpec::Make(NeverSpec{}, "false", rule_name_hint));
    return ResultOk(std::move(spec));
  }
  picojson::object schema_copy = schema;
  for (const auto& type : type_array) {
    if (!type.is<std::string>()) {
      return ResultErr<SchemaError>(
          SchemaErrorType::kInvalidSchema, "type must be a string or an array of strings"
      );
    }
    schema_copy["type"] = type;
    auto type_result =
        Parse(picojson::value(schema_copy), rule_name_hint + "_" + type.get<std::string>());
    if (type_result.IsErr()) return ResultErr(std::move(type_result).UnwrapErr());
    spec.type_schemas.push_back(std::move(type_result).Unwrap());
  }
  return ResultOk(std::move(spec));
}

}  // namespace

std::string RewriteJSONSchemaPatternForFullMatch(const std::string& pattern) {
  if (IsNegatedJSONSchemaPatternUnion(pattern)) return pattern;
  return RewriteJSONSchemaPatternForFullMatchInternal(pattern);
}

Result<FSMWithStartEnd> BuildJSONSchemaPatternFSM(const std::string& pattern, int max_num_states) {
  return BuildJSONSchemaPatternFSMInternal(pattern, max_num_states);
}

// ==================== IndentManager Implementation ====================

IndentManager::IndentManager(
    std::optional<int> indent,
    const std::string& separator,
    bool any_whitespace,
    std::optional<int> max_whitespace_cnt
)
    : any_whitespace_(any_whitespace),
      enable_newline_(indent.has_value()),
      indent_(indent.value_or(0)),
      separator_(separator),
      total_indent_(0),
      is_first_({true}),
      max_whitespace_cnt_(max_whitespace_cnt) {
  if (max_whitespace_cnt.has_value() && max_whitespace_cnt.value() <= 0) {
    XGRAMMAR_LOG(FATAL) << "max_whitespace_cnt must be positive.";
  }
}

void IndentManager::StartIndent() {
  total_indent_ += indent_;
  is_first_.push_back(true);
}

void IndentManager::EndIndent() {
  total_indent_ -= indent_;
  is_first_.pop_back();
}

std::string IndentManager::StartSeparator() {
  if (any_whitespace_) {
    if (!max_whitespace_cnt_.has_value()) {
      return "[ \\n\\t]*";
    } else {
      return "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
    }
  }
  if (!enable_newline_) {
    return "\"\"";
  }
  return "\"\\n" + std::string(total_indent_, ' ') + "\"";
}

std::string IndentManager::MiddleSeparator() {
  if (any_whitespace_) {
    std::string whitespace_part;
    if (!max_whitespace_cnt_.has_value()) {
      whitespace_part = "[ \\n\\t]*";
    } else {
      whitespace_part = "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
    }
    return whitespace_part + " \"" + separator_ + "\" " + whitespace_part;
  }
  if (!enable_newline_) {
    return "\"" + separator_ + "\"";
  }
  return "\"" + separator_ + "\\n" + std::string(total_indent_, ' ') + "\"";
}

std::string IndentManager::EndSeparator() {
  if (any_whitespace_) {
    if (!max_whitespace_cnt_.has_value()) {
      return "[ \\n\\t]*";
    } else {
      return "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
    }
  }
  if (!enable_newline_) {
    return "\"\"";
  }
  return "\"\\n" + std::string(total_indent_ - indent_, ' ') + "\"";
}

std::string IndentManager::EmptySeparator() {
  if (any_whitespace_) {
    if (!max_whitespace_cnt_.has_value()) {
      return "[ \\n\\t]*";
    } else {
      return "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
    }
  }
  return "\"\"";
}

std::string IndentManager::NextSeparator(bool is_end) {
  if (any_whitespace_) {
    if (is_first_.back() || is_end) {
      is_first_.back() = false;
      if (!max_whitespace_cnt_.has_value()) {
        return "[ \\n\\t]*";
      } else {
        return "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
      }
    } else {
      std::string whitespace_part;
      if (!max_whitespace_cnt_.has_value()) {
        whitespace_part = "[ \\n\\t]*";
      } else {
        whitespace_part = "[ \\n\\t]{0," + std::to_string(max_whitespace_cnt_.value()) + "}";
      }
      return whitespace_part + " \"" + separator_ + "\" " + whitespace_part;
    }
  }

  std::string res = "";
  if (!is_first_.back() && !is_end) {
    res += separator_;
  }
  is_first_.back() = false;

  if (enable_newline_) {
    res += "\\n";
  }

  if (!is_end) {
    res += std::string(total_indent_, ' ');
  } else {
    res += std::string(total_indent_ - indent_, ' ');
  }

  return "\"" + res + "\"";
}

// ==================== Static Constants ====================

const std::string JSONSchemaConverter::kBasicAny = "basic_any";
const std::string JSONSchemaConverter::kBasicInteger = "basic_integer";
const std::string JSONSchemaConverter::kBasicNumber = "basic_number";
const std::string JSONSchemaConverter::kBasicString = "basic_string";
const std::string JSONSchemaConverter::kBasicBoolean = "basic_boolean";
const std::string JSONSchemaConverter::kBasicNull = "basic_null";
const std::string JSONSchemaConverter::kBasicArray = "basic_array";
const std::string JSONSchemaConverter::kBasicObject = "basic_object";
const std::string JSONSchemaConverter::kBasicEscape = "basic_escape";
const std::string JSONSchemaConverter::kBasicStringSub = "basic_string_sub";

// ==================== JSONSchemaConverter Implementation ====================

JSONSchemaConverter::JSONSchemaConverter(
    std::optional<int> indent,
    std::optional<std::pair<std::string, std::string>> separators,
    bool any_whitespace,
    std::optional<int> max_whitespace_cnt,
    RefResolver ref_resolver,
    bool any_order,
    RegexFSMCache* regex_fsm_cache,
    bool enable_runtime_json_string_constraints
)
    : indent_manager_(
          indent,
          separators.has_value() ? separators->first
                                 : (any_whitespace ? "," : (indent.has_value() ? "," : ", ")),
          any_whitespace,
          max_whitespace_cnt
      ),
      any_whitespace_(any_whitespace),
      max_whitespace_cnt_(max_whitespace_cnt),
      any_order_(any_order),
      regex_fsm_cache_(regex_fsm_cache),
      enable_runtime_json_string_constraints_(enable_runtime_json_string_constraints),
      ref_resolver_(std::move(ref_resolver)) {
  std::string colon_sep =
      separators.has_value() ? separators->second : (any_whitespace ? ":" : ": ");
  std::string whitespace = GetWhitespacePattern();
  colon_expr_id_ = FormattingExpression(
      any_whitespace ? whitespace + " \"" + colon_sep + "\" " + whitespace : "\"" + colon_sep + "\""
  );
}

Grammar JSONSchemaConverter::Convert(const SchemaSpecPtr& spec) {
  AddBasicRules();

  // Register the root rule for circular reference handling
  // This allows $ref: "#" to resolve to "root"
  int32_t root_rule_id = builder_.AddEmptyRuleWithHint("root");
  std::string root_rule_name = builder_.GetRule(root_rule_id).name;
  uri_to_rule_id_["#"] = root_rule_id;

  // Check if the spec can be directly mapped to an existing rule
  bool indentation_sensitive = IsIndentationSensitive(spec);
  auto cached_rule = GetCache(spec->cache_key, indentation_sensitive);
  if (cached_rule.has_value()) {
    // Root schema matches a basic type, just reference it
    builder_.UpdateRuleBody(root_rule_id, RuleRef(*cached_rule));
  } else {
    // Generate the rule body
    if (!spec->cache_key.empty()) {
      AddCache(spec->cache_key, root_rule_id, indentation_sensitive);
    }
    builder_.UpdateRuleBody(root_rule_id, GenerateFromSpec(spec, root_rule_name));
  }
  return builder_.Get(root_rule_id);
}

void JSONSchemaConverter::AddBasicRules() { AddBasicRules({}); }

void JSONSchemaConverter::AddBasicRules(const std::vector<std::string>& additional_rule_names) {
  std::vector<std::string> basic_rule_names = {
      kBasicEscape,
      kBasicStringSub,
      kBasicAny,
      kBasicInteger,
      kBasicNumber,
      kBasicString,
      kBasicBoolean,
      kBasicNull,
      kBasicArray,
      kBasicObject,
  };
  basic_rule_names.insert(
      basic_rule_names.end(), additional_rule_names.begin(), additional_rule_names.end()
  );
  for (const auto& name : basic_rule_names) {
    builder_.AddEmptyRule(name);
  }
  AddHelperRules();

  // Create basic rules with a temporary indent manager for compact format
  auto saved_indent_manager = indent_manager_;
  indent_manager_ = IndentManager(
      std::nullopt,
      any_whitespace_ ? "," : ", ",
      any_whitespace_,
      any_whitespace_ ? max_whitespace_cnt_ : std::nullopt
  );

  // basic_any - use "{}" as the cache key for empty schema
  auto any_spec = SchemaSpec::Make(AnySpec{}, "{}", kBasicAny);
  builder_.UpdateRuleBody(kBasicAny, GenerateAny(std::get<AnySpec>(any_spec->spec), kBasicAny));
  AddCache("{}", builder_.GetRuleId(kBasicAny));

  // basic_integer - cache_key matches SchemaParser::ComputeCacheKey for {"type": "integer"}
  constexpr const char* kIntegerCacheKey = "{\"type\":\"integer\"}";
  builder_.UpdateRuleBody(kBasicInteger, GenerateInteger(IntegerSpec{}, kBasicInteger));
  AddCache(kIntegerCacheKey, builder_.GetRuleId(kBasicInteger));

  // basic_number - cache_key matches SchemaParser::ComputeCacheKey for {"type": "number"}
  constexpr const char* kNumberCacheKey = "{\"type\":\"number\"}";
  builder_.UpdateRuleBody(kBasicNumber, GenerateNumber(NumberSpec{}, kBasicNumber));
  AddCache(kNumberCacheKey, builder_.GetRuleId(kBasicNumber));

  constexpr const char* kStringCacheKey = "{\"type\":\"string\"}";
  builder_.UpdateRuleBody(kBasicString, Sequence({ByteString("\""), RuleRef(kBasicStringSub)}));
  AddCache(kStringCacheKey, builder_.GetRuleId(kBasicString));

  // basic_boolean - cache_key matches SchemaParser::ComputeCacheKey for {"type": "boolean"}
  constexpr const char* kBooleanCacheKey = "{\"type\":\"boolean\"}";
  builder_.UpdateRuleBody(kBasicBoolean, GenerateBoolean(BooleanSpec{}, kBasicBoolean));
  AddCache(kBooleanCacheKey, builder_.GetRuleId(kBasicBoolean));

  // basic_null - cache_key matches SchemaParser::ComputeCacheKey for {"type": "null"}
  constexpr const char* kNullCacheKey = "{\"type\":\"null\"}";
  builder_.UpdateRuleBody(kBasicNull, GenerateNull(NullSpec{}, kBasicNull));
  AddCache(kNullCacheKey, builder_.GetRuleId(kBasicNull));

  // basic_array - cache_key matches SchemaParser::ComputeCacheKey for {"type": "array"}
  constexpr const char* kArrayCacheKey = "{\"type\":\"array\"}";
  ArraySpec array_spec_val;
  array_spec_val.allow_additional_items = true;
  array_spec_val.additional_items = any_spec;
  builder_.UpdateRuleBody(kBasicArray, GenerateArray(array_spec_val, kBasicArray));
  AddCache(kArrayCacheKey, builder_.GetRuleId(kBasicArray));

  // basic_object - cache_key matches SchemaParser::ComputeCacheKey for {"type": "object"}
  constexpr const char* kObjectCacheKey = "{\"type\":\"object\"}";
  ObjectSpec obj_spec_val;
  obj_spec_val.allow_additional_properties = true;
  obj_spec_val.additional_properties_schema = any_spec;
  builder_.UpdateRuleBody(kBasicObject, GenerateObject(obj_spec_val, kBasicObject));
  AddCache(kObjectCacheKey, builder_.GetRuleId(kBasicObject));

  indent_manager_ = saved_indent_manager;
}

void JSONSchemaConverter::AddHelperRules() {
  if (max_whitespace_cnt_.has_value()) {
    // Preserve historical helper-rule numbering after grammar optimization. The text parser
    // allocated one initial bounded-repetition helper that dead-code elimination later removed.
    builder_.AddRuleWithHint(kBasicStringSub, Empty());
  }
  int32_t escaped_character = builder_.AddCharacterClass(
      {{'"', '"'},
       {'\\', '\\'},
       {'/', '/'},
       {'b', 'b'},
       {'f', 'f'},
       {'n', 'n'},
       {'r', 'r'},
       {'t', 't'}}
  );
  int32_t hexadecimal_character = builder_.AddCharacterClass({{'A', 'F'}, {'a', 'f'}, {'0', '9'}});
  int32_t unicode_escape = Sequence(
      {ByteString("u"),
       hexadecimal_character,
       hexadecimal_character,
       hexadecimal_character,
       hexadecimal_character}
  );
  builder_.UpdateRuleBody(kBasicEscape, Choice({escaped_character, unicode_escape}));

  int32_t normal_character = builder_.AddCharacterClass(
      {{0, 0x1f}, {'"', '"'}, {'\\', '\\'}, {'\r', '\r'}, {'\n', '\n'}}, true
  );
  int32_t string_sub_ref = RuleRef(kBasicStringSub);
  int32_t string_sub_body = Choice(
      {ByteString("\""),
       Sequence({normal_character, string_sub_ref}),
       Sequence({ByteString("\\"), RuleRef(kBasicEscape), string_sub_ref})}
  );
  builder_.UpdateRuleBody(kBasicStringSub, string_sub_body);
  int32_t closing_context =
      builder_.AddCharacterClass({{',', ','}, {'}', '}'}, {']', ']'}, {':', ':'}});
  builder_.UpdateLookaheadAssertion(
      kBasicStringSub, Sequence({WhitespaceExpression(), closing_context})
  );
}

// Keep converter-specific node reuse local; GrammarBuilder creates all AST nodes.
int32_t JSONSchemaConverter::Empty() {
  if (!empty_expr_id_.has_value()) {
    empty_expr_id_ = builder_.AddEmptyStr();
  }
  return *empty_expr_id_;
}

int32_t JSONSchemaConverter::Impossible() {
  if (!impossible_expr_id_.has_value()) {
    impossible_expr_id_ = builder_.AddCharacterClass({});
  }
  return *impossible_expr_id_;
}

int32_t JSONSchemaConverter::ByteString(const std::string& value) {
  auto it = byte_string_expr_ids_.find(value);
  if (it != byte_string_expr_ids_.end()) {
    return it->second;
  }
  int32_t expr_id = value.empty() ? Empty() : builder_.AddByteString(value);
  byte_string_expr_ids_[value] = expr_id;
  return expr_id;
}

int32_t JSONSchemaConverter::TagDispatch(
    bool loop_after_dispatch, std::vector<std::string> excludes
) {
  return builder_.AddTagDispatch(
      Grammar::Impl::TagDispatch{{}, loop_after_dispatch, std::move(excludes)}
  );
}

int32_t JSONSchemaConverter::RuleRef(int32_t rule_id) {
  auto it = rule_ref_expr_ids_.find(rule_id);
  if (it != rule_ref_expr_ids_.end()) {
    return it->second;
  }
  int32_t expr_id = builder_.AddRuleRef(rule_id);
  rule_ref_expr_ids_[rule_id] = expr_id;
  return expr_id;
}

int32_t JSONSchemaConverter::RuleRef(const std::string& rule_name) {
  int32_t rule_id = builder_.GetRuleId(rule_name);
  XGRAMMAR_CHECK(rule_id != -1) << "Rule " << rule_name << " is not allocated";
  return RuleRef(rule_id);
}

int32_t JSONSchemaConverter::Sequence(const std::vector<int32_t>& elements) {
  if (elements.empty()) {
    return Empty();
  }
  if (elements.size() == 1) {
    return elements[0];
  }
  return builder_.AddSequence(elements);
}

int32_t JSONSchemaConverter::Choice(const std::vector<int32_t>& choices) {
  if (choices.empty()) {
    return Empty();
  }
  if (choices.size() == 1) {
    return choices[0];
  }
  return builder_.AddChoices(choices);
}

int32_t JSONSchemaConverter::Repeat(
    const std::string& rule_name_hint, int32_t expr_id, int32_t min_count, int32_t max_count
) {
  if (min_count == 0 && max_count == 0) {
    return Empty();
  }
  if (min_count == 1 && max_count == 1) {
    return expr_id;
  }
  if (min_count == 0 && max_count == 1) {
    return Choice({Empty(), expr_id});
  }
  if (min_count == 0 && max_count == -1) {
    auto expr = builder_.GetGrammarExpr(expr_id);
    if (expr.type == GrammarBuilder::GrammarExprType::kCharacterClass) {
      std::vector<int32_t> data(expr.begin(), expr.end());
      return builder_.AddGrammarExpr(
          {GrammarBuilder::GrammarExprType::kCharacterClassStar,
           data.data(),
           static_cast<int32_t>(data.size())}
      );
    }
  }
  return builder_.AddRepeatFromExpr(rule_name_hint, expr_id, min_count, max_count);
}

int32_t JSONSchemaConverter::AddSubGrammar(const Grammar& grammar) {
  int32_t rule_id = SubGrammarAdder::Apply(&builder_, grammar);
  return RuleRef(rule_id);
}

std::string JSONSchemaConverter::GetWhitespacePattern() const {
  if (!max_whitespace_cnt_.has_value()) {
    return "[ \\n\\t]*";
  }
  return "[ \\n\\t]{0," + std::to_string(*max_whitespace_cnt_) + "}";
}

int32_t JSONSchemaConverter::WhitespaceExpression() {
  std::vector<CharacterClassElement> elements = {{' ', ' '}, {'\n', '\n'}, {'\t', '\t'}};
  if (!max_whitespace_cnt_.has_value()) {
    if (!whitespace_expr_id_.has_value()) {
      whitespace_expr_id_ = builder_.AddCharacterClassStar(elements);
    }
    return *whitespace_expr_id_;
  }
  // Bounded whitespace occurrences intentionally remain distinct, matching the historical
  // parser-produced rule shape after normalization.
  return Repeat(
      "whitespace",
      builder_.AddCharacterClass(elements),
      0,
      static_cast<int32_t>(*max_whitespace_cnt_)
  );
}

int32_t JSONSchemaConverter::FormattingExpression(const std::string& expression) {
  const std::string whitespace = GetWhitespacePattern();
  if (expression == whitespace) {
    return WhitespaceExpression();
  }

  const std::string prefix = whitespace + " ";
  const std::string suffix = " " + whitespace;
  if (expression.size() >= prefix.size() + suffix.size() &&
      expression.compare(0, prefix.size(), prefix) == 0 &&
      expression.compare(expression.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return Sequence(
        {WhitespaceExpression(),
         FormattingExpression(
             expression.substr(prefix.size(), expression.size() - prefix.size() - suffix.size())
         ),
         WhitespaceExpression()}
    );
  }

  picojson::value value;
  std::string error = picojson::parse(value, expression);
  XGRAMMAR_CHECK(error.empty() && value.is<std::string>())
      << "Unsupported indentation expression: " << expression;
  return ByteString(value.get<std::string>());
}

std::string JSONSchemaConverter::NextSeparator(bool is_end) {
  return indent_manager_.NextSeparator(is_end);
}

int32_t JSONSchemaConverter::NextSeparatorExpression(bool is_end) {
  return FormattingExpression(NextSeparator(is_end));
}

std::string JSONSchemaConverter::GetKeyPattern() const { return kBasicString; }

int32_t JSONSchemaConverter::KeyPatternExpression() { return RuleRef(GetKeyPattern()); }

int32_t JSONSchemaConverter::BuildTrieBody(const TrieNode& node, const std::string& rule_name) {
  std::vector<int32_t> choices;
  if (!node.is_terminal) {
    choices.push_back(ByteString("\""));
  }

  std::vector<CharacterClassElement> excluded = {
      {0, 0x1f}, {'"', '"'}, {'\\', '\\'}, {'\r', '\r'}, {'\n', '\n'}
  };
  for (const auto& [character, child] : node.children) {
    static_cast<void>(child);
    excluded.push_back({character, character});
  }
  choices.push_back(Sequence({builder_.AddCharacterClass(excluded, true), RuleRef(kBasicStringSub)})
  );
  choices.push_back(Sequence({ByteString("\\"), RuleRef(kBasicEscape), RuleRef(kBasicStringSub)}));
  for (const auto& [character, child] : node.children) {
    choices.push_back(Sequence(
        {ByteString(std::string(1, static_cast<char>(character))), BuildTrieBody(child, rule_name)}
    ));
  }
  return Choice(choices);
}

int32_t JSONSchemaConverter::BuildTrieContentBody(
    const TrieNode& node, int32_t generic_tail_rule_id
) {
  std::vector<int32_t> choices;
  if (!node.is_terminal) {
    choices.push_back(Empty());
  }

  std::vector<CharacterClassElement> excluded = {
      {0, 0x1f}, {'"', '"'}, {'\\', '\\'}, {'\r', '\r'}, {'\n', '\n'}
  };
  for (const auto& [character, child] : node.children) {
    static_cast<void>(child);
    excluded.push_back({character, character});
  }
  choices.push_back(
      Sequence({builder_.AddCharacterClass(excluded, true), RuleRef(generic_tail_rule_id)})
  );
  // Escaped spellings are decoded by the runtime pattern constraint. As with the historical
  // additional-key trie, an escaped spelling of an excluded ASCII property is not recognized as
  // the same key here; ordinary serializers use the literal spelling and MaskBench instances are
  // canonicalized before matching.
  choices.push_back(
      Sequence({ByteString("\\"), RuleRef(kBasicEscape), RuleRef(generic_tail_rule_id)})
  );
  for (const auto& [character, child] : node.children) {
    choices.push_back(Sequence(
        {ByteString(std::string(1, static_cast<char>(character))),
         BuildTrieContentBody(child, generic_tail_rule_id)}
    ));
  }
  return Choice(choices);
}

int32_t JSONSchemaConverter::PatternPropertyKeyExpression(
    const ObjectSpec::PatternProperty& pattern_property, const std::string& rule_name
) {
  if (pattern_property.excluded_property_names.empty() ||
      !enable_runtime_json_string_constraints_) {
    return Sequence(
        {ByteString("\""), JSONSchemaPatternExpression(pattern_property.pattern), ByteString("\"")}
    );
  }

  TrieNode root;
  for (const auto& property_name : pattern_property.excluded_property_names) {
    TrieNode* current = &root;
    for (unsigned char character : property_name) {
      current = &current->children[character];
    }
    current->is_terminal = true;
  }

  int32_t tail_rule_id = builder_.AddEmptyRuleWithHint(rule_name + "_tail");
  int32_t normal_character = builder_.AddCharacterClass(
      {{0, 0x1f}, {'"', '"'}, {'\\', '\\'}, {'\r', '\r'}, {'\n', '\n'}}, true
  );
  builder_.UpdateRuleBody(
      tail_rule_id,
      Choice(
          {Empty(),
           Sequence({normal_character, RuleRef(tail_rule_id)}),
           Sequence({ByteString("\\"), RuleRef(kBasicEscape), RuleRef(tail_rule_id)})}
      )
  );

  int32_t content_rule_id = builder_.AddEmptyRuleWithHint(rule_name + "_content");
  builder_.UpdateRuleBody(content_rule_id, BuildTrieContentBody(root, tail_rule_id));
  builder_.UpdateJSONStringLengthBounds(content_rule_id, 0, -1);
  builder_.UpdateJSONStringPattern(content_rule_id, pattern_property.pattern);
  return Sequence({ByteString("\""), RuleRef(content_rule_id), ByteString("\"")});
}

int32_t JSONSchemaConverter::AdditionalPropertyKeyExpressionExcludingPatterns(
    const ObjectSpec& spec, const std::string& rule_name
) {
  XGRAMMAR_DCHECK(!spec.pattern_properties.empty());
  if (!enable_runtime_json_string_constraints_) {
    // There is no sound CFG fallback for complementing an arbitrary collection of search
    // patterns. Fail closed instead of letting matching keys bypass their value schemas.
    return Impossible();
  }

  std::vector<std::string> patterns;
  patterns.reserve(spec.pattern_properties.size());
  for (const auto& pattern_property : spec.pattern_properties) {
    patterns.push_back(pattern_property.pattern);
  }
  std::string negated_union = EncodeNegatedJSONSchemaPatternUnion(patterns);
  auto built = BuildJSONSchemaPatternFSM(negated_union, kJSONSchemaPatternDFAStateLimit);
  if (built.IsErr()) {
    XGRAMMAR_LOG(WARNING
    ) << "Cannot complement patternProperties key patterns within the runtime DFA limit; "
         "additionalProperties keys will be rejected";
    return Impossible();
  }

  ObjectSpec::PatternProperty key_constraint;
  key_constraint.pattern = std::move(negated_union);
  key_constraint.excluded_property_names.reserve(spec.properties.size());
  for (const auto& property : spec.properties) {
    key_constraint.excluded_property_names.push_back(property.name);
  }
  return PatternPropertyKeyExpression(key_constraint, rule_name);
}

int32_t JSONSchemaConverter::GetKeyPatternExcluding(
    const std::vector<ObjectSpec::Property>& properties, const std::string& rule_name
) {
  if (properties.empty()) {
    return KeyPatternExpression();
  }

  // Build trie from property names
  // TODO(linzhang): The trie only excludes the literal unescaped spelling of each property name.
  TrieNode root;
  for (const auto& prop : properties) {
    TrieNode* cur = &root;
    for (unsigned char c : prop.name) {
      cur = &cur->children[c];
    }
    cur->is_terminal = true;
  }

  int32_t key_rule_id = builder_.AddEmptyRuleWithHint(rule_name + "_addl_key");
  std::string key_rule_name = builder_.GetRule(key_rule_id).name;
  builder_.UpdateRuleBody(
      key_rule_id, Sequence({ByteString("\""), BuildTrieBody(root, key_rule_name)})
  );
  builder_.UpdateLookaheadAssertion(
      key_rule_id,
      Sequence(
          {WhitespaceExpression(),
           builder_.AddCharacterClass({{',', ','}, {'}', '}'}, {']', ']'}, {':', ':'}})}
      )
  );
  return RuleRef(key_rule_id);
}

std::string JSONSchemaConverter::GetBasicAnyRuleName() const { return kBasicAny; }

void JSONSchemaConverter::AddCache(
    const std::string& key, int32_t rule_id, bool indentation_sensitive
) {
  if (!key.empty()) {
    int64_t indentation_context = indentation_sensitive ? indent_manager_.GetCacheContext() : 0;
    rule_cache_manager_.AddCache(key, 0, indentation_context, rule_id);
  }
}

std::optional<int32_t> JSONSchemaConverter::GetCache(
    const std::string& key, bool indentation_sensitive
) const {
  if (key.empty()) {
    return std::nullopt;
  }
  int64_t indentation_context = indentation_sensitive ? indent_manager_.GetCacheContext() : 0;
  return rule_cache_manager_.GetCache(key, 0, indentation_context);
}

bool JSONSchemaConverter::IsIndentationSensitive(const SchemaSpecPtr& spec) const {
  auto cached = indentation_sensitivity_cache_.find(spec);
  if (cached != indentation_sensitivity_cache_.end()) {
    return cached->second;
  }
  bool result = std::visit(
      [this](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ArraySpec> || std::is_same_v<T, ObjectSpec> ||
                      std::is_same_v<T, RefSpec>) {
          return true;
        } else if constexpr (std::is_same_v<T, ConstSpec>) {
          return !value.json_value.empty() &&
                 (value.json_value.front() == '[' || value.json_value.front() == '{');
        } else if constexpr (std::is_same_v<T, EnumSpec>) {
          return std::any_of(
              value.json_values.begin(),
              value.json_values.end(),
              [](const std::string& json_value) {
                return !json_value.empty() &&
                       (json_value.front() == '[' || json_value.front() == '{');
              }
          );
        } else if constexpr (std::is_same_v<T, AnyOfSpec> || std::is_same_v<T, OneOfSpec>) {
          return std::any_of(
              value.options.begin(),
              value.options.end(),
              [this](const auto& option) { return IsIndentationSensitive(option); }
          );
        } else if constexpr (std::is_same_v<T, AllOfSpec>) {
          return std::any_of(
              value.schemas.begin(),
              value.schemas.end(),
              [this](const auto& schema) { return IsIndentationSensitive(schema); }
          );
        } else if constexpr (std::is_same_v<T, TypeArraySpec>) {
          return std::any_of(
              value.type_schemas.begin(),
              value.type_schemas.end(),
              [this](const auto& schema) { return IsIndentationSensitive(schema); }
          );
        } else {
          return false;
        }
      },
      spec->spec
  );
  indentation_sensitivity_cache_.emplace(spec, result);
  return result;
}

int32_t JSONSchemaConverter::CreateRule(
    const SchemaSpecPtr& spec, const std::string& rule_name_hint
) {
  bool indentation_sensitive = IsIndentationSensitive(spec);
  auto cached = GetCache(spec->cache_key, indentation_sensitive);
  if (cached.has_value()) {
    return cached.value();
  }
  int32_t rule_id = builder_.AddEmptyRuleWithHint(rule_name_hint);
  AddCache(spec->cache_key, rule_id, indentation_sensitive);
  // Copy the name before generating: GenerateFromSpec may add rules and reallocate the
  // builder's rule storage, invalidating references into it.
  std::string rule_name = builder_.GetRule(rule_id).name;
  builder_.UpdateRuleBody(rule_id, GenerateFromSpec(spec, rule_name));
  return rule_id;
}

int32_t JSONSchemaConverter::GenerateFromSpec(
    const SchemaSpecPtr& spec, const std::string& rule_name_hint
) {
  return std::visit(
      [this, &rule_name_hint](const auto& s) -> int32_t {
        using T = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<T, IntegerSpec>) {
          return GenerateInteger(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, NumberSpec>) {
          return GenerateNumber(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, StringSpec>) {
          return GenerateString(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, BooleanSpec>) {
          return GenerateBoolean(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, NullSpec>) {
          return GenerateNull(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, ArraySpec>) {
          return GenerateArray(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, ObjectSpec>) {
          return GenerateObject(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, AnySpec>) {
          return GenerateAny(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, NeverSpec>) {
          return Impossible();
        } else if constexpr (std::is_same_v<T, ConstSpec>) {
          return GenerateConst(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, EnumSpec>) {
          return GenerateEnum(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, RefSpec>) {
          return GenerateRef(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, AnyOfSpec>) {
          return GenerateAnyOf(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, OneOfSpec>) {
          return GenerateOneOf(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, AllOfSpec>) {
          return GenerateAllOf(s, rule_name_hint);
        } else if constexpr (std::is_same_v<T, TypeArraySpec>) {
          return GenerateTypeArray(s, rule_name_hint);
        } else {
          XGRAMMAR_LOG(FATAL) << "Unknown spec type";
        }
      },
      spec->spec
  );
}

/*!
 * \brief Emit the grammar expression matching a regex. Prefer the Regex node with
 * json_string=true so the pattern is compiled into a single automaton by GrammarFSMBuilder;
 * json_string=true excludes the characters that must be escaped in a JSON string ('"', '\\'
 * and the control characters) from every character match, so classes like \S cannot emit an
 * unescaped quote. Fall back to the CFG expansion when the FSM regex engine does not support
 * the pattern, or when the exclusion makes the pattern unmatchable (e.g. a pattern requiring
 * a literal '"').
 */
int32_t JSONSchemaConverter::RegexExpression(
    const std::string& regex, bool json_string, bool force_cfg_expansion
) {
  std::string cache_key;
  cache_key.reserve(regex.size() + 2);
  cache_key.push_back(static_cast<char>(json_string));
  cache_key.push_back(static_cast<char>(force_cfg_expansion));
  cache_key.append(regex);
  const auto cached = regex_expr_ids_.find(cache_key);
  if (cached != regex_expr_ids_.end()) {
    return cached->second;
  }

  bool can_use_fsm = !force_cfg_expansion;
  if (json_string) {
    can_use_fsm =
        can_use_fsm && std::all_of(regex.begin(), regex.end(), [](unsigned char character) {
          return character >= 0x20 && character <= 0x7e;
        });
  }
  if (can_use_fsm) {
    std::optional<FSMWithStartEnd> local_fsm;
    const FSMWithStartEnd* fsm = nullptr;
    std::string fsm_cache_key;
    if (regex_fsm_cache_ != nullptr) {
      fsm_cache_key = MakeRegexFSMCacheKey(regex, json_string);
      const auto cached_fsm = regex_fsm_cache_->find(fsm_cache_key);
      if (cached_fsm != regex_fsm_cache_->end()) {
        fsm = &cached_fsm->second;
      }
    }
    if (fsm == nullptr) {
      auto fsm_result = GrammarFSMBuilder::Regex(regex, json_string);
      if (fsm_result.IsOk()) {
        if (regex_fsm_cache_ != nullptr) {
          const auto inserted =
              regex_fsm_cache_->emplace(std::move(fsm_cache_key), std::move(fsm_result).Unwrap());
          fsm = &inserted.first->second;
        } else {
          local_fsm.emplace(std::move(fsm_result).Unwrap());
          fsm = &*local_fsm;
        }
      }
    }
    if (fsm != nullptr) {
      std::unordered_set<int> reachable_states;
      fsm->GetReachableStates(&reachable_states);
      bool language_is_empty =
          std::none_of(reachable_states.begin(), reachable_states.end(), [&](int state) {
            return fsm->IsEndState(state);
          });
      if (!language_is_empty) {
        const int32_t result = builder_.AddRegex(regex, json_string);
        regex_expr_ids_.emplace(std::move(cache_key), result);
        return result;
      }
    }
  }

  // Keep regex conversion independent. Only the uncommon fallback path converts its existing
  // EBNF result to a subgrammar; the JSON Schema rule graph itself is still built directly.
  const int32_t result = AddSubGrammar(Grammar::FromEBNF(RegexToEBNF(regex)));
  regex_expr_ids_.emplace(std::move(cache_key), result);
  return result;
}

int32_t JSONSchemaConverter::JSONSchemaPatternExpression(
    const std::string& regex, std::optional<std::pair<int32_t, int32_t>> decoded_length_bounds
) {
  std::string cache_key = regex;
  if (decoded_length_bounds.has_value()) {
    cache_key.push_back('\0');
    cache_key += "length:" + std::to_string(decoded_length_bounds->first) + ":" +
                 std::to_string(decoded_length_bounds->second);
  }
  const auto cached = json_schema_pattern_expr_ids_.find(cache_key);
  if (cached != json_schema_pattern_expr_ids_.end()) {
    return cached->second;
  }

  auto emit_regex_pattern = [&](const std::string& effective_regex) {
    const std::string rewritten = RewriteJSONSchemaPatternForFullMatch(effective_regex);
    int32_t result = RegexExpression(rewritten, /*json_string=*/true);
    if (regex_fsm_cache_ != nullptr) {
      auto cached_fsm =
          regex_fsm_cache_->find(MakeRegexFSMCacheKey(rewritten, /*json_string=*/true));
      if (cached_fsm != regex_fsm_cache_->end() && !cached_fsm->second.IsDFA()) {
        auto dfa = cached_fsm->second.ToDFA(kJSONSchemaPatternDFAStateLimit);
        if (dfa.IsOk()) {
          cached_fsm->second = std::move(dfa).Unwrap();
        }
      }
    }
    return result;
  };

  if (auto simple_repeat = ParseSimpleCharacterClassRepeat(regex)) {
    int32_t min_count = simple_repeat->min_count;
    int32_t max_count = simple_repeat->max_count;
    if (decoded_length_bounds.has_value()) {
      min_count = std::max(min_count, decoded_length_bounds->first);
      if (max_count == -1 ||
          (decoded_length_bounds->second != -1 && decoded_length_bounds->second < max_count)) {
        max_count = decoded_length_bounds->second;
      }
      if (max_count != -1 && min_count > max_count) {
        int32_t result = Impossible();
        json_schema_pattern_expr_ids_.emplace(std::move(cache_key), result);
        return result;
      }
    }

    std::vector<int32_t> encoded_character_choices;

    std::vector<CharacterClassElement> raw_elements;
    for (int byte = 0; byte < 256;) {
      const bool allowed =
          simple_repeat->allowed_bytes.test(byte) && byte >= 0x20 && byte != '"' && byte != '\\';
      if (!allowed) {
        ++byte;
        continue;
      }
      int range_end = byte;
      while (range_end + 1 < 256 && simple_repeat->allowed_bytes.test(range_end + 1) &&
             range_end + 1 >= 0x20 && range_end + 1 != '"' && range_end + 1 != '\\') {
        ++range_end;
      }
      raw_elements.push_back({byte, range_end});
      byte = range_end + 1;
    }
    if (!raw_elements.empty()) {
      encoded_character_choices.push_back(builder_.AddCharacterClass(raw_elements));
    }

    static constexpr std::pair<int, char> kShortEscapes[] = {
        {'"', '"'},
        {'\\', '\\'},
        {'/', '/'},
        {'\b', 'b'},
        {'\f', 'f'},
        {'\n', 'n'},
        {'\r', 'r'},
        {'\t', 't'},
    };
    for (const auto& escaped : kShortEscapes) {
      if (simple_repeat->allowed_bytes.test(escaped.first)) {
        encoded_character_choices.push_back(ByteString(std::string{'\\', escaped.second}));
      }
    }

    for (int high = 0; high <= 7; ++high) {
      std::vector<CharacterClassElement> low_nibble_spellings;
      for (int low = 0; low <= 15; ++low) {
        if (!simple_repeat->allowed_bytes.test((high << 4) | low)) {
          continue;
        }
        if (low <= 9) {
          low_nibble_spellings.push_back({'0' + low, '0' + low});
        } else {
          low_nibble_spellings.push_back({'A' + low - 10, 'A' + low - 10});
          low_nibble_spellings.push_back({'a' + low - 10, 'a' + low - 10});
        }
      }
      if (!low_nibble_spellings.empty()) {
        encoded_character_choices.push_back(Sequence(
            {ByteString("\\u00" + std::string(1, static_cast<char>('0' + high))),
             builder_.AddCharacterClass(low_nibble_spellings)}
        ));
      }
    }

    int32_t encoded_character = Choice(encoded_character_choices);
    int32_t result =
        Repeat("json_schema_pattern_character", encoded_character, min_count, max_count);
    json_schema_pattern_expr_ids_.emplace(std::move(cache_key), result);
    return result;
  }

  // A decoded-byte DFA can be evaluated as a side constraint while a small generic JSON string
  // grammar handles the source spelling. The regex FSM builder emits complete canonical UTF-8
  // sequences for Unicode atoms, so raw UTF-8, short escapes, \uXXXX, and surrogate pairs all
  // advance the same automaton after decoding. Keeping the DFA out of the main grammar also avoids
  // copying large literal-alternative automata into every parser and mask-cache state.
  const std::string rewritten = RewriteJSONSchemaPatternForFullMatch(regex);
  if (enable_runtime_json_string_constraints_) {
    std::optional<FSMWithStartEnd> local_fsm;
    FSMWithStartEnd* fsm = nullptr;
    const std::string fsm_cache_key = MakeRegexFSMCacheKey(rewritten, /*json_string=*/false);
    if (regex_fsm_cache_ != nullptr) {
      auto cached_fsm = regex_fsm_cache_->find(fsm_cache_key);
      if (cached_fsm != regex_fsm_cache_->end()) {
        fsm = &cached_fsm->second;
      }
    }
    if (fsm == nullptr) {
      auto built = BuildJSONSchemaPatternFSMInternal(regex, kJSONSchemaPatternDFAStateLimit);
      if (built.IsOk()) {
        local_fsm.emplace(std::move(built).Unwrap());
        fsm = &*local_fsm;
      }
    }
    if (fsm != nullptr && !fsm->IsDFA()) {
      auto dfa = fsm->ToDFA(kJSONSchemaPatternDFAStateLimit);
      if (dfa.IsOk()) {
        *fsm = std::move(dfa).Unwrap();
      } else {
        fsm = nullptr;
      }
    }
    if (fsm != nullptr && fsm->IsDFA()) {
      if (regex_fsm_cache_ != nullptr && local_fsm.has_value()) {
        auto inserted = regex_fsm_cache_->emplace(fsm_cache_key, std::move(*local_fsm));
        fsm = &inserted.first->second;
      }
      int32_t normal_character = builder_.AddCharacterClass(
          {{0, 0x1f}, {'"', '"'}, {'\\', '\\'}, {'\r', '\r'}, {'\n', '\n'}}, true
      );
      int32_t rule_id = builder_.AddEmptyRuleWithHint("json_schema_streaming_pattern");
      int32_t rule_ref = RuleRef(rule_id);
      int32_t body = Choice(
          {Empty(),
           Sequence({normal_character, rule_ref}),
           Sequence({ByteString("\\"), RuleRef(kBasicEscape), rule_ref})}
      );
      builder_.UpdateRuleBody(rule_id, body);
      const auto bounds = decoded_length_bounds.value_or(std::make_pair(0, -1));
      builder_.UpdateJSONStringLengthBounds(rule_id, bounds.first, bounds.second);
      builder_.UpdateJSONStringPattern(rule_id, regex);
      int32_t result = RuleRef(rule_id);
      json_schema_pattern_expr_ids_.emplace(std::move(cache_key), result);
      return result;
    }
  }

  int32_t result = emit_regex_pattern(regex);
  if (decoded_length_bounds.has_value()) {
    int32_t rule_id = builder_.AddRuleWithHint("json_schema_pattern_length", result);
    builder_.UpdateJSONStringLengthBounds(
        rule_id, decoded_length_bounds->first, decoded_length_bounds->second
    );
    result = RuleRef(rule_id);
  }
  json_schema_pattern_expr_ids_.emplace(std::move(cache_key), result);
  return result;
}

// ==================== Generate Methods ====================

int32_t JSONSchemaConverter::GenerateInteger(
    const IntegerSpec& spec, const std::string& rule_name
) {
  // Shared with ParseInteger's range validation so emission and validation agree on the effective
  // range; a nullopt side means that side is unbounded.
  const EffectiveIntegerRange range = ComputeEffectiveIntegerRange(spec);
  std::optional<std::string> start = range.start;
  std::optional<std::string> end = range.end;

  if (spec.multiple_of.has_value()) {
    if (start.has_value() && end.has_value()) {
      auto start_int64 = TryConvertToInt64(*start);
      auto end_int64 = TryConvertToInt64(*end);
      if (start_int64.has_value() && end_int64.has_value() &&
          !IsRangeWidthOverCap(*start_int64, *end_int64, kIntegerMultipleOfRangeWidthMax)) {
        std::vector<int32_t> multiples;
        for (int64_t value = *start_int64; value <= *end_int64; ++value) {
          if (IsMultipleOf(value, *spec.multiple_of)) {
            multiples.push_back(ByteString(std::to_string(value)));
          }
          if (value == std::numeric_limits<int64_t>::max()) {
            break;
          }
        }
        return Choice(multiples);
      }
    }
    if (!start.has_value() && !end.has_value()) {
      return GenerateIntegerMultipleOfDFA(*spec.multiple_of, rule_name);
    }
  }
  int32_t result;
  if (start.has_value() || end.has_value()) {
    result = RegexExpression(
        GenerateRangeRegex(start, end),
        false,
        /*force_cfg_expansion=*/true
    );
  } else {
    int32_t optional_minus = Choice({Empty(), ByteString("-")});
    result = Choice(
        {ByteString("0"),
         Sequence(
             {optional_minus,
              builder_.AddCharacterClass({{'1', '9'}}),
              builder_.AddCharacterClassStar({{'0', '9'}})}
         )}
    );
  }
  if (!spec.multiple_of.has_value()) {
    return result;
  }
  int32_t constrained_rule = builder_.AddRuleWithHint(rule_name + "_multiple_of", result);
  builder_.UpdateJSONNumberMultipleOf(
      constrained_rule, static_cast<int32_t>(*spec.multiple_of), /*decimal_scale=*/0
  );
  return RuleRef(constrained_rule);
}

int32_t JSONSchemaConverter::GenerateIntegerMultipleOfDFA(
    int64_t multiple_of, const std::string& rule_name
) {
  std::vector<int32_t> states(multiple_of);
  for (int64_t state = 0; state < multiple_of; ++state) {
    states[state] = builder_.AddEmptyRuleWithHint(
        rule_name + "_multiple_of_" + std::to_string(multiple_of) + "_mod_" + std::to_string(state)
    );
  }
  for (int64_t state = 0; state < multiple_of; ++state) {
    std::vector<int32_t> transitions;
    if (state == 0) {
      transitions.push_back(Empty());
    }
    for (int64_t digit = 0; digit <= 9; ++digit) {
      int64_t next_state = (state * 10 + digit) % multiple_of;
      transitions.push_back(
          Sequence({ByteString(std::to_string(digit)), RuleRef(states[next_state])})
      );
    }
    builder_.UpdateRuleBody(states[state], Choice(transitions));
  }

  std::vector<int32_t> non_zero_starts;
  for (int64_t digit = 1; digit <= 9; ++digit) {
    non_zero_starts.push_back(
        Sequence({ByteString(std::to_string(digit)), RuleRef(states[digit % multiple_of])})
    );
  }
  return Choice(
      {ByteString("0"), Sequence({Choice({Empty(), ByteString("-")}), Choice(non_zero_starts)})}
  );
}

int32_t JSONSchemaConverter::GenerateNumber(const NumberSpec& spec, const std::string& rule_name) {
  std::optional<std::string> start = spec.minimum;
  std::optional<std::string> end = spec.maximum;
  bool exclusive_start = false;
  bool exclusive_end = false;
  // When both bounds are present the larger lower bound wins; on a tie the
  // exclusive one is stricter.
  if (spec.exclusive_minimum.has_value() &&
      (!start.has_value() || CompareExactDecimalLexemes(*spec.exclusive_minimum, *start) >= 0)) {
    start = spec.exclusive_minimum;
    exclusive_start = true;
  }
  if (spec.exclusive_maximum.has_value() &&
      (!end.has_value() || CompareExactDecimalLexemes(*spec.exclusive_maximum, *end) <= 0)) {
    end = spec.exclusive_maximum;
    exclusive_end = true;
  }
  const bool has_range = start.has_value() || end.has_value();
  const bool has_runtime_constraint = has_range || spec.multiple_of.has_value();
  int32_t result;
  if (has_runtime_constraint && rule_name != kBasicNumber) {
    // Runtime metadata validates exact decimal bounds and multipleOf jointly. Reuse the structural
    // JSON-number language so arbitrary precision and scientific notation remain representable.
    result = RuleRef(kBasicNumber);
  } else {
    int32_t optional_minus = Choice({Empty(), ByteString("-")});
    int32_t integer_part = Choice(
        {ByteString("0"),
         Sequence(
             {builder_.AddCharacterClass({{'1', '9'}}), builder_.AddCharacterClassStar({{'0', '9'}})
             }
         )}
    );
    int32_t one_or_more_digits =
        Repeat(rule_name + "_digits", builder_.AddCharacterClass({{'0', '9'}}), 1, -1);
    int32_t fraction = Choice({Empty(), Sequence({ByteString("."), one_or_more_digits})});
    int32_t exponent = Choice(
        {Empty(),
         Sequence(
             {builder_.AddCharacterClass({{'e', 'e'}, {'E', 'E'}}),
              Choice({Empty(), builder_.AddCharacterClass({{'+', '+'}, {'-', '-'}})}),
              one_or_more_digits}
         )}
    );
    // Note: The format must be "-"? ("0" | ...) not ("0" | "-"? ...).
    result = Sequence({optional_minus, integer_part, fraction, exponent});
  }
  if (!has_runtime_constraint) {
    return result;
  }
  int32_t constrained_rule = builder_.AddRuleWithHint(rule_name + "_number_constraint", result);
  builder_.UpdateJSONNumberRange(
      constrained_rule, start.value_or(""), end.value_or(""), exclusive_start, exclusive_end
  );
  if (spec.multiple_of.has_value()) {
    builder_.UpdateJSONNumberMultipleOf(
        constrained_rule, spec.multiple_of->first, spec.multiple_of->second
    );
  }
  return RuleRef(constrained_rule);
}

int32_t JSONSchemaConverter::GenerateString(const StringSpec& spec, const std::string& rule_name) {
  const bool has_length = spec.min_length != 0 || spec.max_length != -1;
  std::optional<std::string> compact_pattern;
  if (!spec.format.has_value() && spec.pattern.has_value() && has_length) {
    if (ParseSimpleCharacterClassRepeat(*spec.pattern).has_value()) {
      compact_pattern = *spec.pattern;
    } else if (spec.min_length == spec.max_length) {
      compact_pattern = AnchorExactLengthCharacterClassSearch(*spec.pattern, spec.min_length);
    }
  }
  if (compact_pattern.has_value()) {
    return Sequence(
        {ByteString("\""),
         JSONSchemaPatternExpression(
             *compact_pattern, std::make_pair(spec.min_length, spec.max_length)
         ),
         ByteString("\"")}
    );
  }
  // General patterns validate decoded JSON length in the same helper that owns the pattern. This
  // avoids a pattern-by-length product and lets eligible search patterns use the streaming path.
  if (!spec.format.has_value() && spec.pattern.has_value() && has_length) {
    return Sequence(
        {ByteString("\""),
         JSONSchemaPatternExpression(
             *spec.pattern, std::make_pair(spec.min_length, spec.max_length)
         ),
         ByteString("\"")}
    );
  }
  // Check for format
  if (spec.format.has_value()) {
    auto regex = JSONFormatToRegexPattern(*spec.format);
    if (regex.has_value()) {
      // The built-in format regexes use constructs that the FSM regex engine does not fully
      // support yet (e.g. quoted email local parts), so they keep the CFG expansion.
      int32_t content = RegexExpression(*regex, false, true);
      const bool needs_runtime_constraint = spec.pattern.has_value() || has_length;
      bool can_stream_pattern = !spec.pattern.has_value();
      if (spec.pattern.has_value() && enable_runtime_json_string_constraints_) {
        auto built =
            BuildJSONSchemaPatternFSMInternal(*spec.pattern, kJSONSchemaPatternDFAStateLimit);
        can_stream_pattern = built.IsOk() && built.ValueRef().IsDFA();
      }
      if (needs_runtime_constraint && enable_runtime_json_string_constraints_ &&
          can_stream_pattern) {
        int32_t constrained_rule =
            builder_.AddRuleWithHint("json_schema_format_constraints", content);
        builder_.UpdateJSONStringLengthBounds(constrained_rule, spec.min_length, spec.max_length);
        if (spec.pattern.has_value()) {
          builder_.UpdateJSONStringPattern(constrained_rule, *spec.pattern);
        }
        content = RuleRef(constrained_rule);
      }
      return Sequence({ByteString("\""), content, ByteString("\"")});
    }
  }
  // Check for pattern
  if (spec.pattern.has_value()) {
    return Sequence({ByteString("\""), JSONSchemaPatternExpression(*spec.pattern), ByteString("\"")}
    );
  }
  // Check for length constraints
  if (has_length) {
    if (!enable_runtime_json_string_constraints_) {
      // Runtime decoded-length metadata is intentionally absent from the public EBNF text
      // format. Preserve the historical text representation so converting a Schema to EBNF and
      // parsing it again does not silently drop the length bound.
      int32_t character = builder_.AddCharacterClass(
          {{0, 0x1f}, {'"', '"'}, {'\\', '\\'}, {'\r', '\r'}, {'\n', '\n'}}, true
      );
      int32_t body = Repeat(rule_name + "_characters", character, spec.min_length, spec.max_length);
      return Sequence({ByteString("\""), body, ByteString("\"")});
    }
    int32_t normal_character = builder_.AddCharacterClass(
        {{0, 0x1f}, {'"', '"'}, {'\\', '\\'}, {'\r', '\r'}, {'\n', '\n'}}, true
    );
    int32_t content_rule = builder_.AddEmptyRuleWithHint(rule_name + "_decoded_length");
    int32_t content_ref = RuleRef(content_rule);
    builder_.UpdateRuleBody(
        content_rule,
        Choice(
            {Empty(),
             Sequence({normal_character, content_ref}),
             Sequence({ByteString("\\"), RuleRef(kBasicEscape), content_ref})}
        )
    );
    builder_.UpdateJSONStringLengthBounds(content_rule, spec.min_length, spec.max_length);
    return Sequence({ByteString("\""), content_ref, ByteString("\"")});
  }
  // Default string
  return Sequence({ByteString("\""), RuleRef(kBasicStringSub)});
}

int32_t JSONSchemaConverter::GenerateBoolean(
    const BooleanSpec& spec, const std::string& rule_name
) {
  return Choice({ByteString("true"), ByteString("false")});
}

int32_t JSONSchemaConverter::GenerateNull(const NullSpec& spec, const std::string& rule_name) {
  return ByteString("null");
}

int32_t JSONSchemaConverter::GenerateArray(const ArraySpec& spec, const std::string& rule_name) {
  indent_manager_.StartIndent();
  int32_t start_separator = FormattingExpression(indent_manager_.StartSeparator());
  int32_t middle_separator = FormattingExpression(indent_manager_.MiddleSeparator());
  int32_t end_separator = FormattingExpression(indent_manager_.EndSeparator());
  int32_t empty_separator = FormattingExpression(indent_manager_.EmptySeparator());

  std::vector<int32_t> item_rule_ids;
  for (size_t index = 0; index < spec.prefix_items.size(); ++index) {
    item_rule_ids.push_back(
        CreateRule(spec.prefix_items[index], rule_name + "_item_" + std::to_string(index))
    );
  }
  int32_t additional_rule_id = -1;
  if (spec.allow_additional_items && spec.additional_items) {
    additional_rule_id = CreateRule(spec.additional_items, rule_name + "_additional");
  }
  indent_manager_.EndIndent();

  int32_t left_bracket = ByteString("[");
  int32_t right_bracket = ByteString("]");
  int32_t empty_array = Sequence({left_bracket, empty_separator, right_bracket});

  if (item_rule_ids.empty()) {
    if (!spec.allow_additional_items || spec.max_items == 0) {
      return empty_array;
    }
    int32_t additional = RuleRef(additional_rule_id);
    int32_t tail = Repeat(
        rule_name + "_items",
        Sequence({middle_separator, additional}),
        spec.min_items == 0 ? 0 : static_cast<int32_t>(spec.min_items - 1),
        spec.max_items == -1 ? -1 : static_cast<int32_t>(spec.max_items - 1)
    );
    int32_t nonempty =
        Sequence({left_bracket, start_separator, additional, tail, end_separator, right_bracket});
    return spec.min_items == 0 ? Choice({nonempty, empty_array}) : nonempty;
  }

  // prefixItems constrains positions that are present; it does not require every prefix position
  // to exist. Build the suffix backwards so each feasible array length can close at its current
  // position without duplicating all preceding item expressions.
  const int64_t prefix_size = static_cast<int64_t>(item_rule_ids.size());
  std::vector<int32_t> tails(item_rule_ids.size() + 1, Impossible());
  std::vector<int32_t> terminal_choices;
  if (prefix_size >= spec.min_items && (spec.max_items == -1 || prefix_size <= spec.max_items)) {
    terminal_choices.push_back(Sequence({end_separator, right_bracket}));
  }
  if (spec.allow_additional_items && additional_rule_id != -1 &&
      (spec.max_items == -1 || prefix_size < spec.max_items)) {
    int64_t minimum_additional = std::max(int64_t{1}, spec.min_items - prefix_size);
    int64_t maximum_additional = spec.max_items == -1 ? -1 : spec.max_items - prefix_size;
    int32_t additional_tail = Repeat(
        rule_name + "_additional_items",
        Sequence({middle_separator, RuleRef(additional_rule_id)}),
        static_cast<int32_t>(minimum_additional - 1),
        maximum_additional == -1 ? -1 : static_cast<int32_t>(maximum_additional - 1)
    );
    terminal_choices.push_back(Sequence(
        {middle_separator,
         RuleRef(additional_rule_id),
         additional_tail,
         end_separator,
         right_bracket}
    ));
  }
  tails.back() = terminal_choices.empty() ? Impossible() : Choice(terminal_choices);

  for (size_t index = item_rule_ids.size(); index-- > 0;) {
    int64_t consumed = static_cast<int64_t>(index);
    std::vector<int32_t> choices;
    if (consumed >= spec.min_items && (spec.max_items == -1 || consumed <= spec.max_items)) {
      choices.push_back(
          index == 0 ? Sequence({empty_separator, right_bracket})
                     : Sequence({end_separator, right_bracket})
      );
    }
    if (spec.max_items == -1 || consumed < spec.max_items) {
      choices.push_back(Sequence(
          {index == 0 ? start_separator : middle_separator,
           RuleRef(item_rule_ids[index]),
           tails[index + 1]}
      ));
    }
    tails[index] = choices.empty() ? Impossible() : Choice(choices);
  }
  return Sequence({left_bracket, tails[0]});
}

int32_t JSONSchemaConverter::FormatPropertyKey(const std::string& key) {
  return ByteString(picojson::value(key).serialize());
}

int32_t JSONSchemaConverter::FormatProperty(
    const std::string& key, int32_t value_rule_id, const std::string& rule_name, int64_t idx
) {
  return Sequence({FormatPropertyKey(key), colon_expr_id_, RuleRef(value_rule_id)});
}

int32_t JSONSchemaConverter::FormatOtherProperty(
    int32_t key_pattern_expr,
    int32_t value_rule_id,
    const std::string& rule_name,
    const std::string& rule_name_suffix
) {
  return Sequence({key_pattern_expr, colon_expr_id_, RuleRef(value_rule_id)});
}

int32_t JSONSchemaConverter::GetPropertyWithNumberConstraints(
    int32_t pattern,
    int min_properties,
    int max_properties,
    int already_repeated_times,
    const std::string& rule_name
) {
  if (max_properties != -1 && max_properties == already_repeated_times) {
    return Empty();
  }
  int lower = std::max(0, min_properties - already_repeated_times);
  int upper = max_properties == -1 ? -1 : std::max(-1, max_properties - already_repeated_times);
  return Repeat(rule_name + "_properties", pattern, lower, upper);
}

int32_t JSONSchemaConverter::GetAnyOrderRuleForProperties(
    const std::vector<ObjectSpec::Property>& properties,
    const std::unordered_set<std::string>& required,
    const SchemaSpecPtr& additional,
    const std::string& rule_name,
    const std::string& additional_suffix,
    int min_properties,
    int max_properties,
    const std::optional<int32_t>& additional_property_override
) {
  int32_t first_separator = NextSeparatorExpression();
  int32_t middle_separator = NextSeparatorExpression();
  int32_t last_separator = NextSeparatorExpression(true);

  std::vector<int32_t> items;
  std::vector<int32_t> required_bits;
  std::unordered_map<std::string, int32_t> required_name_to_bit;
  for (const auto& required_name : required) {
    required_name_to_bit.emplace(required_name, static_cast<int32_t>(required_name_to_bit.size()));
  }
  std::unordered_set<std::string> represented_required_names;
  for (size_t index = 0; index < properties.size(); ++index) {
    const auto& property = properties[index];
    int32_t value_rule_id =
        CreateRule(property.schema, rule_name + "_prop_" + std::to_string(index));
    items.push_back(FormatProperty(property.name, value_rule_id, rule_name, index));
    auto required_it = required_name_to_bit.find(property.name);
    if (required_it == required_name_to_bit.end()) {
      required_bits.push_back(-1);
    } else {
      required_bits.push_back(required_it->second);
      represented_required_names.insert(property.name);
    }
  }
  if (additional != nullptr) {
    if (additional_property_override.has_value()) {
      items.push_back(*additional_property_override);
    } else {
      int32_t value_rule_id = CreateRule(additional, rule_name + "_" + additional_suffix);
      items.push_back(FormatOtherProperty(
          GetKeyPatternExcluding(properties, rule_name), value_rule_id, rule_name, additional_suffix
      ));
    }
    required_bits.push_back(-1);
  }

  const bool all_required_are_named =
      !required.empty() && represented_required_names.size() == required.size();
  bool can_track_required =
      all_required_are_named && required.size() <= kAnyOrderRequiredPropertyLimit;
  size_t mask_state_count = can_track_required ? size_t{1} << required.size() : 0;
  bool can_track_without_count = can_track_required && max_properties == -1 &&
                                 min_properties <= static_cast<int>(required.size()) &&
                                 mask_state_count <= kAnyOrderRequiredSubsetStateLimit;

  if (can_track_without_count) {
    // Once every distinct required key has appeared, the minimum property count is necessarily
    // satisfied. Keep the first-property transition outside the subset graph so the graph needs
    // one state per required-key mask rather than a second dimension merely recording
    // empty/non-empty. This makes twelve required keys fit exactly in the existing 4,096-state
    // budget and halves the common smaller graphs as well.
    const size_t full_mask = mask_state_count - 1;
    std::vector<int32_t> state_rule_ids;
    state_rule_ids.reserve(mask_state_count);
    for (size_t mask = 0; mask < mask_state_count; ++mask) {
      state_rule_ids.push_back(builder_.AddEmptyRuleWithHint(rule_name + "_required_state"));
    }

    auto next_mask = [&](size_t mask, size_t item_index) {
      if (required_bits[item_index] >= 0) {
        mask |= size_t{1} << required_bits[item_index];
      }
      return mask;
    };
    for (size_t mask = 0; mask < mask_state_count; ++mask) {
      std::vector<int32_t> choices;
      if (mask == full_mask) choices.push_back(last_separator);
      for (size_t item_index = 0; item_index < items.size(); ++item_index) {
        choices.push_back(Sequence(
            {middle_separator,
             items[item_index],
             RuleRef(state_rule_ids[next_mask(mask, item_index)])}
        ));
      }
      builder_.UpdateRuleBody(state_rule_ids[mask], Choice(choices));
    }

    std::vector<int32_t> first_choices;
    first_choices.reserve(items.size());
    for (size_t item_index = 0; item_index < items.size(); ++item_index) {
      first_choices.push_back(Sequence(
          {first_separator, items[item_index], RuleRef(state_rule_ids[next_mask(0, item_index)])}
      ));
    }
    return Choice(first_choices);
  }

  size_t count_state_count = 0;
  int saturated_count = -1;
  if (can_track_required) {
    if (max_properties == -1) {
      saturated_count = std::max(1, min_properties);
      count_state_count = static_cast<size_t>(saturated_count) + 1;
    } else {
      count_state_count = static_cast<size_t>(max_properties) + 1;
    }
    can_track_required = count_state_count != 0 &&
                         mask_state_count <= kAnyOrderRequiredSubsetStateLimit / count_state_count;
  }

  if (can_track_required) {
    const size_t full_mask = mask_state_count - 1;
    const size_t state_count = mask_state_count * count_state_count;
    std::vector<int32_t> state_rule_ids;
    state_rule_ids.reserve(state_count);
    for (size_t state = 0; state < state_count; ++state) {
      state_rule_ids.push_back(builder_.AddEmptyRuleWithHint(rule_name + "_required_state"));
    }
    auto state_id = [&](size_t mask, size_t count) {
      return state_rule_ids[mask * count_state_count + count];
    };

    for (size_t mask = 0; mask < mask_state_count; ++mask) {
      for (size_t count = 0; count < count_state_count; ++count) {
        std::vector<int32_t> choices;
        if (mask == full_mask && static_cast<int>(count) >= min_properties) {
          choices.push_back(last_separator);
        }

        bool may_add_property = max_properties == -1 || static_cast<int>(count) < max_properties;
        if (may_add_property) {
          size_t next_count = count + 1;
          if (max_properties == -1 && next_count > static_cast<size_t>(saturated_count)) {
            next_count = static_cast<size_t>(saturated_count);
          }
          int32_t separator = count == 0 ? first_separator : middle_separator;
          for (size_t item_index = 0; item_index < items.size(); ++item_index) {
            size_t next_mask = mask;
            if (required_bits[item_index] >= 0) {
              next_mask |= size_t{1} << required_bits[item_index];
            }
            choices.push_back(
                Sequence({separator, items[item_index], RuleRef(state_id(next_mask, next_count))})
            );
          }
        }
        builder_.UpdateRuleBody(
            state_id(mask, count), choices.empty() ? Impossible() : Choice(choices)
        );
      }
    }
    return RuleRef(state_id(0, 0));
  }

  // Large named required sets use a compact runtime bitset. A zero-width marker after each
  // required property sets its owning object's bit, and the owner rule may complete only after
  // every required key has appeared. Keeping the property itself inline lets the shared-choice
  // trie merge common key prefixes just as it does for objects without runtime required state;
  // wrapping the whole property in a metadata-bearing rule would leave one live parser branch per
  // property throughout every value. This avoids both the exponential subset grammar and the old
  // count-only approximation. The textual EBNF conversion cannot preserve rule metadata, so it
  // retains the legacy bounded fallback; direct Grammar/compiler APIs use the exact runtime
  // constraint.
  if (enable_runtime_json_string_constraints_ && all_required_are_named) {
    int32_t owner_rule_id = builder_.AddEmptyRuleWithHint(rule_name + "_required_runtime");
    std::vector<int32_t> marked_items;
    marked_items.reserve(items.size());
    for (size_t item_index = 0; item_index < items.size(); ++item_index) {
      if (required_bits[item_index] < 0) {
        marked_items.push_back(items[item_index]);
        continue;
      }

      const auto& item = builder_.GetGrammarExpr(items[item_index]);
      std::vector<int32_t> elements;
      if (item.type == GrammarBuilder::GrammarExprType::kSequence) {
        elements.assign(item.begin(), item.end());
      } else {
        elements.push_back(items[item_index]);
      }
      int32_t marker_rule_id = builder_.AddRuleWithHint(
          rule_name + "_required_property_" + std::to_string(item_index), Empty()
      );
      builder_.UpdateJSONObjectRequiredProperty(
          marker_rule_id, owner_rule_id, required_bits[item_index]
      );
      elements.push_back(RuleRef(marker_rule_id));
      marked_items.push_back(Sequence(elements));
    }
    int32_t item_rule_id = builder_.AddRuleWithHint(rule_name + "_item", Choice(marked_items));
    int minimum_count = std::max(min_properties, static_cast<int>(required.size()));
    int32_t repeated_items = GetPropertyWithNumberConstraints(
        Sequence({middle_separator, RuleRef(item_rule_id)}),
        minimum_count,
        max_properties,
        1,
        rule_name
    );
    builder_.UpdateRuleBody(
        owner_rule_id,
        Sequence({first_separator, RuleRef(item_rule_id), repeated_items, last_separator})
    );
    builder_.UpdateJSONObjectRequiredCount(owner_rule_id, static_cast<int32_t>(required.size()));
    return RuleRef(owner_rule_id);
  }

  // Bounded fallback for required names that cannot be represented by named-property wrappers
  // (for example, a required key admitted only by patternProperties).
  int32_t item_rule_id = builder_.AddRuleWithHint(rule_name + "_item", Choice(items));

  int minimum_count = std::max(min_properties, static_cast<int>(required.size()));
  int32_t repeated_items = GetPropertyWithNumberConstraints(
      Sequence({middle_separator, RuleRef(item_rule_id)}),
      minimum_count,
      max_properties,
      1,
      rule_name
  );
  return Sequence({first_separator, RuleRef(item_rule_id), repeated_items, last_separator});
}

int32_t JSONSchemaConverter::GetPartialRuleForProperties(
    const std::vector<ObjectSpec::Property>& properties,
    const std::unordered_set<std::string>& required,
    const SchemaSpecPtr& additional,
    const std::string& rule_name,
    const std::string& additional_suffix,
    int min_properties,
    int max_properties,
    const std::optional<int32_t>& additional_property_override
) {
  if (max_properties == 0) {
    return Empty();
  }
  if (any_order_) {
    return GetAnyOrderRuleForProperties(
        properties,
        required,
        additional,
        rule_name,
        additional_suffix,
        min_properties,
        max_properties,
        additional_property_override
    );
  }

  int32_t first_separator = NextSeparatorExpression();
  int32_t middle_separator = NextSeparatorExpression();
  int32_t last_separator = NextSeparatorExpression(true);

  std::vector<int32_t> property_patterns;
  for (size_t index = 0; index < properties.size(); ++index) {
    int32_t value_rule_id =
        CreateRule(properties[index].schema, rule_name + "_prop_" + std::to_string(index));
    property_patterns.push_back(
        FormatProperty(properties[index].name, value_rule_id, rule_name, index)
    );
  }

  bool allow_additional = additional != nullptr;
  std::optional<int32_t> additional_pattern;
  auto get_additional_pattern = [&]() -> int32_t {
    if (!additional_pattern.has_value()) {
      if (additional_property_override.has_value()) {
        additional_pattern = *additional_property_override;
      } else {
        int32_t value_rule_id = CreateRule(additional, rule_name + "_" + additional_suffix);
        additional_pattern = FormatOtherProperty(
            GetKeyPatternExcluding(properties, rule_name),
            value_rule_id,
            rule_name,
            additional_suffix
        );
      }
    }
    return *additional_pattern;
  };

  if (min_properties == 0 && max_properties == -1) {
    // Case 1: No property number constraints
    std::vector<int32_t> tails(properties.size(), Empty());
    std::vector<uint8_t> is_required(properties.size(), false);

    if (allow_additional) {
      int32_t repeated_additional = Repeat(
          rule_name + "_additional_properties",
          Sequence({middle_separator, get_additional_pattern()}),
          0,
          -1
      );
      int32_t tail_rule_id = builder_.AddRuleWithHint(
          rule_name + "_part_" + std::to_string(static_cast<int>(properties.size()) - 1),
          repeated_additional
      );
      tails.back() = RuleRef(tail_rule_id);
    }

    for (int index = static_cast<int>(properties.size()) - 2; index >= 0; --index) {
      int32_t with_property =
          Sequence({middle_separator, property_patterns[index + 1], tails[index + 1]});
      int32_t body = with_property;
      if (!required.count(properties[index + 1].name)) {
        body = Choice({tails[index + 1], with_property});
      } else {
        is_required[index + 1] = true;
      }
      int32_t tail_rule_id =
          builder_.AddRuleWithHint(rule_name + "_part_" + std::to_string(index), body);
      tails[index] = RuleRef(tail_rule_id);
    }
    if (required.count(properties[0].name)) {
      is_required[0] = true;
    }

    std::vector<int32_t> choices;
    for (size_t index = 0; index < properties.size(); ++index) {
      choices.push_back(Sequence({property_patterns[index], tails[index]}));
      if (is_required[index]) {
        break;
      }
    }
    if (allow_additional && required.empty()) {
      choices.push_back(Sequence({get_additional_pattern(), tails.back()}));
    }
    return Sequence({first_separator, Choice(choices), last_separator});
  }

  const int property_count = static_cast<int>(properties.size());
  std::vector<uint8_t> is_required(property_count, false);
  std::vector<int> matched_min(property_count, 0);
  bool found_required = required.count(properties[0].name);
  matched_min[0] = 1;
  for (int index = 1; index < property_count; ++index) {
    if (required.count(properties[index].name)) {
      is_required[index] = true;
      matched_min[index] = matched_min[index - 1] + 1;
    } else {
      matched_min[index] = matched_min[index - 1];
    }
    if (!found_required) {
      matched_min[index] = 1;
    }
    if (is_required[index]) {
      found_required = true;
    }
  }
  if (required.count(properties[0].name)) {
    is_required[0] = true;
  }

  if (max_properties == -1) {
    // Case 2: With constraint on the lower bound of the properties number
    std::vector<std::vector<int32_t>> tails(property_count);
    matched_min.back() = allow_additional ? std::max(1, matched_min.back())
                                          : std::max(min_properties, matched_min.back());
    for (int index = property_count - 2; index >= 0; --index) {
      matched_min[index] = std::max(matched_min[index], matched_min[index + 1] - 1);
    }

    for (int matched = matched_min.back(); matched <= property_count; ++matched) {
      int32_t body = allow_additional ? GetPropertyWithNumberConstraints(
                                            Sequence({middle_separator, get_additional_pattern()}),
                                            min_properties,
                                            max_properties,
                                            matched,
                                            rule_name
                                        )
                                      : Empty();
      if (allow_additional) {
        int32_t tail_rule_id = builder_.AddRuleWithHint(
            rule_name + "_part_" + std::to_string(property_count - 1) + "_" +
                std::to_string(matched),
            body
        );
        tails.back().push_back(RuleRef(tail_rule_id));
      } else {
        tails.back().push_back(body);
      }
    }

    for (int index = property_count - 2; index >= 0; --index) {
      for (int matched = matched_min[index]; matched <= index + 1; ++matched) {
        int32_t with_property = Sequence(
            {middle_separator,
             property_patterns[index + 1],
             tails[index + 1][matched + 1 - matched_min[index + 1]]}
        );
        int32_t body =
            (is_required[index + 1] || matched == matched_min[index + 1] - 1)
                ? with_property
                : Choice({tails[index + 1][matched - matched_min[index + 1]], with_property});
        int32_t tail_rule_id = builder_.AddRuleWithHint(
            rule_name + "_part_" + std::to_string(index) + "_" + std::to_string(matched), body
        );
        tails[index].push_back(RuleRef(tail_rule_id));
      }
    }

    std::vector<int32_t> choices;
    for (int index = 0; index < property_count; ++index) {
      if (matched_min[index] > 1) {
        break;
      }
      choices.push_back(Sequence({property_patterns[index], tails[index][1 - matched_min[index]]}));
      if (is_required[index]) {
        break;
      }
    }
    if (allow_additional && required.empty()) {
      choices.push_back(Sequence(
          {get_additional_pattern(),
           GetPropertyWithNumberConstraints(
               Sequence({middle_separator, get_additional_pattern()}),
               min_properties,
               max_properties,
               1,
               rule_name
           )}
      ));
    }
    return Sequence({first_separator, Choice(choices), last_separator});
  }

  // Case 3: With constraints on both lower & upper bound of the properties number
  std::vector<std::vector<int32_t>> tails(property_count);
  std::vector<int> matched_max(property_count, property_count);
  matched_max[0] = 1;
  for (int index = 1; index < property_count; ++index) {
    matched_max[index] = matched_max[index - 1] + 1;
  }
  matched_min.back() = allow_additional ? std::max(1, matched_min.back())
                                        : std::max(min_properties, matched_min.back());
  matched_max.back() = std::min(max_properties, matched_max.back());
  for (int index = property_count - 2; index >= 0; --index) {
    matched_min[index] = std::max(matched_min[index], matched_min[index + 1] - 1);
    matched_max[index] = is_required[index + 1]
                             ? std::min(matched_max[index], matched_max[index + 1] - 1)
                             : std::min(matched_max[index], matched_max[index + 1]);
  }

  for (int matched = matched_min.back(); matched <= matched_max.back(); ++matched) {
    int32_t body = allow_additional ? GetPropertyWithNumberConstraints(
                                          Sequence({middle_separator, get_additional_pattern()}),
                                          min_properties,
                                          max_properties,
                                          matched,
                                          rule_name
                                      )
                                    : Empty();
    if (allow_additional) {
      int32_t tail_rule_id = builder_.AddRuleWithHint(
          rule_name + "_part_" + std::to_string(property_count - 1) + "_" + std::to_string(matched),
          body
      );
      tails.back().push_back(RuleRef(tail_rule_id));
    } else {
      tails.back().push_back(body);
    }
  }

  for (int index = property_count - 2; index >= 0; --index) {
    for (int matched = matched_min[index]; matched <= matched_max[index]; ++matched) {
      int32_t body;
      if (matched == matched_max[index + 1]) {
        body = tails[index + 1][matched - matched_min[index + 1]];
      } else {
        int32_t with_property = Sequence(
            {middle_separator,
             property_patterns[index + 1],
             tails[index + 1][matched + 1 - matched_min[index + 1]]}
        );
        body = (is_required[index + 1] || matched == matched_min[index + 1] - 1)
                   ? with_property
                   : Choice({tails[index + 1][matched - matched_min[index + 1]], with_property});
      }
      int32_t tail_rule_id = builder_.AddRuleWithHint(
          rule_name + "_part_" + std::to_string(index) + "_" + std::to_string(matched), body
      );
      tails[index].push_back(RuleRef(tail_rule_id));
    }
  }

  std::vector<int32_t> choices;
  for (int index = 0; index < property_count; ++index) {
    if (matched_max[index] < matched_min[index]) {
      continue;
    }
    if (matched_min[index] > 1) {
      break;
    }
    choices.push_back(Sequence({property_patterns[index], tails[index][1 - matched_min[index]]}));
    if (is_required[index]) {
      break;
    }
  }
  if (allow_additional && required.empty()) {
    choices.push_back(Sequence(
        {get_additional_pattern(),
         GetPropertyWithNumberConstraints(
             Sequence({middle_separator, get_additional_pattern()}),
             min_properties,
             max_properties,
             1,
             rule_name
         )}
    ));
  }
  return Sequence({first_separator, Choice(choices), last_separator});
}

int32_t JSONSchemaConverter::GenerateObject(
    const ObjectSpec& spec, const std::string& rule_name, bool need_braces
) {
  if (spec.property_names && std::holds_alternative<NeverSpec>(spec.property_names->spec)) {
    if (!spec.required.empty() || spec.min_properties > 0) {
      return Impossible();
    }
    int32_t empty_content = any_whitespace_ ? WhitespaceExpression() : Empty();
    return need_braces ? Sequence({ByteString("{"), empty_content, ByteString("}")})
                       : empty_content;
  }

  // Determine additional property handling
  std::string additional_suffix;
  SchemaSpecPtr additional_property;
  if (spec.allow_additional_properties && spec.additional_properties_schema) {
    additional_suffix = "addl";
    additional_property = spec.additional_properties_schema;
  } else if (spec.allow_unevaluated_properties && spec.unevaluated_properties_schema) {
    additional_suffix = "uneval";
    additional_property = spec.unevaluated_properties_schema;
  } else if (spec.allow_additional_properties || spec.allow_unevaluated_properties) {
    additional_suffix = "addl";
    additional_property = SchemaSpec::Make(AnySpec{}, "", "any");
  }

  indent_manager_.StartIndent();
  bool has_content = false;
  bool could_be_empty = false;
  int32_t content = Empty();

  if (!spec.properties.empty() && (!spec.pattern_properties.empty() || spec.property_names)) {
    // Case 1a: properties coexist with patternProperties and/or propertyNames.
    // Use GetPartialRuleForProperties for named properties, and build
    // patternProperties/propertyNames as the additional property pattern override.
    SchemaSpecPtr effective_additional = additional_property;
    std::string effective_suffix = additional_suffix;
    std::optional<int32_t> additional_override;

    if (!spec.pattern_properties.empty()) {
      // Build patternProperties as additional property alternatives
      std::vector<int32_t> patterns;
      for (size_t index = 0; index < spec.pattern_properties.size(); ++index) {
        const auto& pattern_property = spec.pattern_properties[index];
        int32_t value_rule_id =
            CreateRule(pattern_property.schema, rule_name + "_pp_" + std::to_string(index));
        patterns.push_back(Sequence(
            {PatternPropertyKeyExpression(
                 pattern_property, rule_name + "_pp_key_" + std::to_string(index)
             ),
             colon_expr_id_,
             RuleRef(value_rule_id)}
        ));
      }
      // Merge with existing additionalProperties if present
      if (effective_additional) {
        int32_t value_rule_id =
            CreateRule(effective_additional, rule_name + "_" + effective_suffix);
        patterns.push_back(FormatOtherProperty(
            AdditionalPropertyKeyExpressionExcludingPatterns(spec, rule_name + "_addl_key"),
            value_rule_id,
            rule_name,
            effective_suffix
        ));
      }
      additional_override = Choice(patterns);
      if (!effective_additional) {
        effective_additional = SchemaSpec::Make(AnySpec{}, "", "any");
      }
      effective_suffix = "pp";
    } else if (spec.property_names && effective_additional) {
      // propertyNames constrains keys of additional properties.
      // Only apply when additional properties are allowed - when additionalProperties
      // is false, no extra keys beyond named properties should be permitted.
      int32_t key_rule_id = CreateRule(spec.property_names, rule_name + "_name");
      int32_t value_rule_id = CreateRule(effective_additional, rule_name + "_" + effective_suffix);
      additional_override =
          Sequence({RuleRef(key_rule_id), colon_expr_id_, RuleRef(value_rule_id)});
      effective_suffix = "pn";
    }

    content = GetPartialRuleForProperties(
        spec.properties,
        spec.required,
        effective_additional,
        rule_name,
        effective_suffix,
        spec.min_properties,
        spec.max_properties,
        additional_override
    );
    has_content = spec.max_properties != 0;
    could_be_empty = spec.required.empty() && spec.min_properties == 0;
  } else if (!spec.pattern_properties.empty() || spec.property_names) {
    // Case 1b: patternProperties or propertyNames without named properties
    if (spec.max_properties != 0) {
      int32_t beginning_separator = NextSeparatorExpression();
      std::vector<int32_t> property_choices;
      if (!spec.pattern_properties.empty()) {
        for (size_t index = 0; index < spec.pattern_properties.size(); ++index) {
          const auto& pattern_property = spec.pattern_properties[index];
          int32_t value_rule_id =
              CreateRule(pattern_property.schema, rule_name + "_prop_" + std::to_string(index));
          property_choices.push_back(Sequence(
              {beginning_separator,
               PatternPropertyKeyExpression(
                   pattern_property, rule_name + "_pp_key_" + std::to_string(index)
               ),
               colon_expr_id_,
               RuleRef(value_rule_id)}
          ));
        }
        // additionalProperties applies only to keys not covered by any patternProperties entry.
        if (additional_property) {
          int32_t value_rule_id =
              CreateRule(additional_property, rule_name + "_" + additional_suffix);
          property_choices.push_back(Sequence(
              {beginning_separator,
               FormatOtherProperty(
                   AdditionalPropertyKeyExpressionExcludingPatterns(spec, rule_name + "_addl_key"),
                   value_rule_id,
                   rule_name,
                   additional_suffix
               )}
          ));
        }
      } else {
        int32_t key_rule_id = CreateRule(spec.property_names, rule_name + "_name");
        property_choices.push_back(Sequence(
            {beginning_separator,
             RuleRef(key_rule_id),
             colon_expr_id_,
             RuleRef(GetBasicAnyRuleName())}
        ));
      }

      int32_t property_rule_id =
          builder_.AddRuleWithHint(rule_name + "_prop", Choice(property_choices));
      int32_t subsequent_property =
          Sequence({NextSeparatorExpression(), RuleRef(property_rule_id)});
      content = Sequence(
          {RuleRef(property_rule_id),
           GetPropertyWithNumberConstraints(
               subsequent_property, spec.min_properties, spec.max_properties, 1, rule_name
           ),
           NextSeparatorExpression(true)}
      );
      has_content = true;
      could_be_empty = spec.min_properties == 0;
    } else {
      could_be_empty = true;
    }
  } else if (!spec.properties.empty()) {
    // Case 2: properties defined (no patternProperties/propertyNames)
    content = GetPartialRuleForProperties(
        spec.properties,
        spec.required,
        additional_property,
        rule_name,
        additional_suffix,
        spec.min_properties,
        spec.max_properties
    );
    has_content = spec.max_properties != 0;
    could_be_empty = spec.required.empty() && spec.min_properties == 0;
  } else if (additional_property) {
    // Case 3: no properties defined, additional properties allowed
    if (spec.max_properties != 0) {
      int32_t value_rule_id = CreateRule(additional_property, rule_name + "_" + additional_suffix);
      int32_t property =
          FormatOtherProperty(KeyPatternExpression(), value_rule_id, rule_name, additional_suffix);
      content = Sequence(
          {NextSeparatorExpression(),
           property,
           GetPropertyWithNumberConstraints(
               Sequence({NextSeparatorExpression(), property}),
               spec.min_properties,
               spec.max_properties,
               1,
               rule_name
           ),
           NextSeparatorExpression(true)}
      );
      has_content = true;
    }
    could_be_empty = spec.min_properties == 0;
  } else {
    // Case 4: no properties, no additional properties, no pattern properties
    // The object is unconditionally empty.
    could_be_empty = true;
  }

  indent_manager_.EndIndent();

  int32_t result = need_braces ? Sequence({ByteString("{"), content, ByteString("}")}) : content;
  if (could_be_empty) {
    int32_t empty_content = any_whitespace_ ? WhitespaceExpression() : Empty();
    int32_t empty_result =
        need_braces ? Sequence({ByteString("{"), empty_content, ByteString("}")}) : empty_content;
    return has_content ? Choice({result, empty_result}) : empty_result;
  }
  return result;
}

int32_t JSONSchemaConverter::GenerateAny(const AnySpec& spec, const std::string& rule_name) {
  return Choice(
      {RuleRef(kBasicNumber),
       RuleRef(kBasicString),
       RuleRef(kBasicBoolean),
       RuleRef(kBasicNull),
       RuleRef(kBasicArray),
       RuleRef(kBasicObject)}
  );
}

std::string JSONSchemaConverter::SerializeExactJSONValue(const picojson::value& value) {
  if (auto number = DecodeExactJSONNumber(value)) {
    return *number;
  }
  if (auto string = DecodeEscapedExactJSONNumberString(value)) {
    return picojson::value(*string).serialize(false);
  }
  if (value.is<picojson::array>()) {
    std::string result = "[";
    const auto& array = value.get<picojson::array>();
    for (size_t index = 0; index < array.size(); ++index) {
      if (index != 0) result.push_back(',');
      result += SerializeExactJSONValue(array[index]);
    }
    result.push_back(']');
    return result;
  }
  if (value.is<picojson::object>()) {
    std::string result = "{";
    const auto& object = value.get<picojson::object>();
    bool first = true;
    for (const auto& key : object.ordered_keys()) {
      if (!first) result.push_back(',');
      first = false;
      result += picojson::value(key).serialize(false);
      result.push_back(':');
      result += SerializeExactJSONValue(object.at(key));
    }
    result.push_back('}');
    return result;
  }
  return value.serialize(false);
}

int32_t JSONSchemaConverter::GenerateJSONValue(
    const picojson::value& value, const std::string& rule_name
) {
  if (auto number = DecodeExactJSONNumber(value)) {
    NumberSpec exact_number;
    exact_number.minimum = *number;
    exact_number.maximum = *number;
    return GenerateNumber(exact_number, rule_name + "_exact_number");
  }
  if (auto string = DecodeEscapedExactJSONNumberString(value)) {
    return ByteString(picojson::value(*string).serialize());
  }
  if (value.is<picojson::array>()) {
    const auto& array = value.get<picojson::array>();
    indent_manager_.StartIndent();
    int32_t start_separator = FormattingExpression(indent_manager_.StartSeparator());
    int32_t middle_separator = FormattingExpression(indent_manager_.MiddleSeparator());
    int32_t end_separator = FormattingExpression(indent_manager_.EndSeparator());
    int32_t empty_separator = FormattingExpression(indent_manager_.EmptySeparator());
    std::vector<int32_t> elements;
    for (size_t index = 0; index < array.size(); ++index) {
      elements.push_back(index == 0 ? start_separator : middle_separator);
      elements.push_back(
          GenerateJSONValue(array[index], rule_name + "_item_" + std::to_string(index))
      );
    }
    elements.push_back(array.empty() ? empty_separator : end_separator);
    indent_manager_.EndIndent();
    return Sequence({ByteString("["), Sequence(elements), ByteString("]")});
  }
  if (value.is<picojson::object>()) {
    const auto& object = value.get<picojson::object>();
    indent_manager_.StartIndent();
    int32_t start_separator = FormattingExpression(indent_manager_.StartSeparator());
    int32_t middle_separator = FormattingExpression(indent_manager_.MiddleSeparator());
    int32_t end_separator = FormattingExpression(indent_manager_.EndSeparator());
    int32_t empty_separator = FormattingExpression(indent_manager_.EmptySeparator());
    std::vector<std::pair<std::string, const picojson::value*>> properties;
    properties.reserve(object.size());
    for (const auto& [key, property_value] : object) {
      properties.emplace_back(key, &property_value);
    }
    std::vector<int32_t> property_expressions;
    property_expressions.reserve(properties.size());
    for (size_t index = 0; index < properties.size(); ++index) {
      property_expressions.push_back(Sequence(
          {FormatPropertyKey(properties[index].first),
           colon_expr_id_,
           GenerateJSONValue(
               *properties[index].second, rule_name + "_prop_" + std::to_string(index)
           )}
      ));
    }
    int32_t content;
    if (properties.empty()) {
      content = empty_separator;
    } else if (any_order_) {
      // A finite object value requires every exact property exactly once. Build a subset DAG so
      // order is free without allowing omissions or duplicates.
      XGRAMMAR_CHECK(properties.size() <= kFiniteJSONValueAnyOrderPropertyLimit)
          << "A const/enum object with " << properties.size()
          << " properties exceeds the exact any_order limit of "
          << kFiniteJSONValueAnyOrderPropertyLimit;
      const size_t state_count = size_t{1} << properties.size();
      std::vector<int32_t> states(state_count, -1);
      states.back() = end_separator;
      for (size_t mask = state_count - 1; mask-- > 0;) {
        std::vector<int32_t> choices;
        for (size_t index = 0; index < properties.size(); ++index) {
          if ((mask & (size_t{1} << index)) == 0) {
            choices.push_back(Sequence(
                {mask == 0 ? start_separator : middle_separator,
                 property_expressions[index],
                 states[mask | (size_t{1} << index)]}
            ));
          }
        }
        states[mask] = Choice(choices);
      }
      content = states[0];
    } else {
      std::vector<int32_t> elements;
      for (size_t index = 0; index < properties.size(); ++index) {
        elements.push_back(index == 0 ? start_separator : middle_separator);
        elements.push_back(property_expressions[index]);
      }
      elements.push_back(end_separator);
      content = Sequence(elements);
    }
    indent_manager_.EndIndent();
    return Sequence({ByteString("{"), content, ByteString("}")});
  }
  return ByteString(value.serialize());
}

int32_t JSONSchemaConverter::GenerateConst(const ConstSpec& spec, const std::string& rule_name) {
  picojson::value value;
  std::string error = picojson::parse(value, spec.json_value);
  XGRAMMAR_CHECK(error.empty()) << "Invalid serialized const value";
  return GenerateJSONValue(value, rule_name);
}

int32_t JSONSchemaConverter::GenerateEnum(const EnumSpec& spec, const std::string& rule_name) {
  XGRAMMAR_DCHECK(!spec.json_values.empty())
      << "GenerateEnum called with empty enum spec for rule: " << rule_name;
  std::vector<int32_t> values;
  values.reserve(spec.json_values.size());
  for (size_t index = 0; index < spec.json_values.size(); ++index) {
    picojson::value value;
    std::string error = picojson::parse(value, spec.json_values[index]);
    XGRAMMAR_CHECK(error.empty()) << "Invalid serialized enum value";
    values.push_back(GenerateJSONValue(value, rule_name + "_value_" + std::to_string(index)));
  }
  return Choice(values);
}

int32_t JSONSchemaConverter::GenerateRef(const RefSpec& spec, const std::string& rule_name) {
  // First check if we have a direct URI mapping (for circular references)
  if (uri_to_rule_id_.count(spec.uri)) {
    return RuleRef(uri_to_rule_id_[spec.uri]);
  }

  if (!ref_resolver_) {
    XGRAMMAR_LOG(FATAL) << "Ref resolver not set; cannot resolve $ref: " << spec.uri;
  }

  // Derive the rule name from the URI path, then resolve before allocating so an
  // equivalent rule can be reused. Register the new rule before generating its body
  // to prevent recursion when the resolved target refers back to this URI.
  std::string rule_name_hint = "ref";
  if (spec.uri.size() >= 2 && spec.uri[0] == '#' && spec.uri[1] == '/') {
    std::string new_rule_name_prefix;
    std::stringstream ss(spec.uri.substr(2));
    std::string part;
    while (std::getline(ss, part, '/')) {
      if (!part.empty()) {
        if (!new_rule_name_prefix.empty()) {
          new_rule_name_prefix += "_";
        }
        for (char c : part) {
          if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
            new_rule_name_prefix += c;
          }
        }
      }
    }
    if (!new_rule_name_prefix.empty()) {
      rule_name_hint = std::move(new_rule_name_prefix);
    }
  }

  SchemaSpecPtr resolved = ref_resolver_(spec.uri, rule_name_hint);
  bool indentation_sensitive = IsIndentationSensitive(resolved);
  auto cached = GetCache(resolved->cache_key, indentation_sensitive);
  if (cached.has_value()) {
    uri_to_rule_id_[spec.uri] = *cached;
    return RuleRef(*cached);
  }

  int32_t allocated_rule_id = builder_.AddEmptyRuleWithHint(rule_name_hint);
  std::string allocated_rule_name = builder_.GetRule(allocated_rule_id).name;
  uri_to_rule_id_[spec.uri] = allocated_rule_id;
  AddCache(resolved->cache_key, allocated_rule_id, indentation_sensitive);
  builder_.UpdateRuleBody(allocated_rule_id, GenerateFromSpec(resolved, allocated_rule_name));
  return RuleRef(allocated_rule_id);
}

int32_t JSONSchemaConverter::GenerateAnyOf(const AnyOfSpec& spec, const std::string& rule_name) {
  std::vector<int32_t> choices;
  for (size_t index = 0; index < spec.options.size(); ++index) {
    choices.push_back(
        RuleRef(CreateRule(spec.options[index], rule_name + "_case_" + std::to_string(index)))
    );
  }
  return Choice(choices);
}

int32_t JSONSchemaConverter::GenerateOneOf(const OneOfSpec& spec, const std::string& rule_name) {
  std::vector<int32_t> choices;
  for (size_t index = 0; index < spec.options.size(); ++index) {
    choices.push_back(
        RuleRef(CreateRule(spec.options[index], rule_name + "_case_" + std::to_string(index)))
    );
  }
  return Choice(choices);
}

int32_t JSONSchemaConverter::GenerateAllOf(const AllOfSpec& spec, const std::string& rule_name) {
  if (std::any_of(spec.schemas.begin(), spec.schemas.end(), [](const SchemaSpecPtr& schema) {
        return std::holds_alternative<NeverSpec>(schema->spec);
      })) {
    return Impossible();
  }
  if (spec.schemas.size() == 1) {
    return GenerateFromSpec(spec.schemas[0], rule_name + "_case_0");
  }
  XGRAMMAR_LOG(WARNING) << "Support for allOf with multiple options is still ongoing";
  return GenerateFromSpec(SchemaSpec::Make(AnySpec{}, "", "any"), rule_name);
}

int32_t JSONSchemaConverter::GenerateTypeArray(
    const TypeArraySpec& spec, const std::string& rule_name
) {
  std::vector<int32_t> choices;
  for (size_t index = 0; index < spec.type_schemas.size(); ++index) {
    choices.push_back(
        RuleRef(CreateRule(spec.type_schemas[index], rule_name + "_type_" + std::to_string(index)))
    );
  }
  return Choice(choices);
}

// ==================== Static Helper Methods ====================

std::optional<std::string> JSONSchemaConverter::JSONFormatToRegexPattern(const std::string& format
) {
  static const auto regex_map = []() -> std::unordered_map<std::string, std::string> {
    std::unordered_map<std::string, std::string> m;

    std::string atext = "[\\w!#$%&'*+/=?^`{|}~-]";
    std::string dot_string = "(" + atext + "+(\\." + atext + "+)*)";
    std::string quoted_string =
        "\\\\\"(\\\\[\\x20-\\x7E]|[\\x20\\x21\\x23-\\x5B\\x5D-\\x7E])*\\\\\"";
    std::string domain =
        "([A-Za-z0-9]([\\-A-Za-z0-9]*[A-Za-z0-9])?)((\\.[A-Za-z0-9][\\-A-Za-z0-9]*[A-Za-z0-9])*"
        ")";
    m["email"] = "^(" + dot_string + "|" + quoted_string + ")@" + domain + "$";

    // RFC 3339 full-date uses the proleptic Gregorian calendar. Keep the four-digit year
    // language (including year 0000, which XGrammar has historically accepted), but reject
    // impossible month/day combinations. February 29 is admitted only for years divisible by
    // four, excluding century years unless they are divisible by 400.
    std::string leap_year =
        "([0-9]{2}(0[48]|[2468][048]|[13579][26])|([02468][048]|[13579][26])00)";
    std::string common_date =
        "[0-9]{4}-((0[13578]|1[02])-(0[1-9]|[12][0-9]|3[01])|"
        "(0[469]|11)-(0[1-9]|[12][0-9]|30)|02-(0[1-9]|1[0-9]|2[0-8]))";
    std::string full_date = "(" + common_date + "|" + leap_year + "-02-29)";
    m["date"] = "^" + full_date + "$";
    m["time"] =
        "^([01]\\d|2[0-3]):[0-5]\\d:([0-5]\\d|60)(\\.\\d+)?(Z|[+-]([01]\\d|2[0-3]):[0-5]\\d)$";
    m["date-time"] =
        "^" + full_date +
        "T([01]\\d|2[0-3]):[0-5]\\d:([0-5]\\d|60)(\\.\\d+)?(Z|[+-]([01]\\d|2[0-3]):[0-5]\\d)$";
    m["duration"] =
        "^P((\\d+D|\\d+M(\\d+D)?|\\d+Y(\\d+M(\\d+D)?)?)(T(\\d+S|\\d+M(\\d+S)?|\\d+H(\\d+M(\\d+"
        "S)?"
        ")?))?|T(\\d+S|\\d+M(\\d+S)?|\\d+H(\\d+M(\\d+S)?)?)|\\d+W)$";

    std::string decbyte = "(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)";
    m["ipv4"] = "^(" + decbyte + "\\.){3}" + decbyte + "$";

    m["ipv6"] =
        "("
        "([0-9a-fA-F]{1,4}:){7,7}[0-9a-fA-F]{1,4}|"
        "([0-9a-fA-F]{1,4}:){1,7}:|"
        "([0-9a-fA-F]{1,4}:){1,6}:[0-9a-fA-F]{1,4}|"
        "([0-9a-fA-F]{1,4}:){1,5}(:[0-9a-fA-F]{1,4}){1,2}|"
        "([0-9a-fA-F]{1,4}:){1,4}(:[0-9a-fA-F]{1,4}){1,3}|"
        "([0-9a-fA-F]{1,4}:){1,3}(:[0-9a-fA-F]{1,4}){1,4}|"
        "([0-9a-fA-F]{1,4}:){1,2}(:[0-9a-fA-F]{1,4}){1,5}|"
        "[0-9a-fA-F]{1,4}:((:[0-9a-fA-F]{1,4}){1,6})|"
        ":((:[0-9a-fA-F]{1,4}){1,7}|:)|"
        "::(ffff(:0{1,4}){0,1}:){0,1}"
        "((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\\.){3,3}"
        "(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])|"
        "([0-9a-fA-F]{1,4}:){1,4}:"
        "((25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])\\.){3,3}"
        "(25[0-5]|(2[0-4]|1{0,1}[0-9]){0,1}[0-9])"
        ")";

    m["hostname"] = "^([a-z0-9]([a-z0-9-]*[a-z0-9])?)(\\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)*$";
    m["uuid"] = "^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$";

    std::string schema_pat = "[a-zA-Z][a-zA-Z+\\.-]*";
    std::string pchar = "([\\w\\.~!$&'()*+,;=:@-]|%[0-9A-Fa-f][0-9A-Fa-f])";
    std::string query_fragment_char = "([\\w\\.~!$&'()*+,;=:@/\\?-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    std::string query = "(\\?" + query_fragment_char + ")?";
    std::string fragment = "(#" + query_fragment_char + ")?";
    std::string path_abempty = "(/" + pchar + "*)*";
    std::string path_absolute_rootless_empty = "/?(" + pchar + "+(/" + pchar + "*)*)?";
    std::string userinfo = "([\\w\\.~!$&'()*+,;=:-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    std::string host = "([\\w\\.~!$&'()*+,;=-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    std::string authority = "(" + userinfo + "@)?" + host + "(:\\d*)?";
    std::string hier_part =
        "(//" + authority + path_abempty + "|" + path_absolute_rootless_empty + ")";
    m["uri"] = "^" + schema_pat + ":" + hier_part + query + fragment + "$";

    pchar = "([\\w\\.~!$&'()*+,;=:@-]|%[0-9A-Fa-f][0-9A-Fa-f])";
    query_fragment_char = "([\\w\\.~!$&'()*+,;=:@/\\?-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    query = "(\\?" + query_fragment_char + ")?";
    fragment = "(#" + query_fragment_char + ")?";
    path_abempty = "(/" + pchar + "*)*";
    std::string path_absolute = "/(" + pchar + "+(/" + pchar + "*)*)?";
    std::string segment_nz_nc = "([\\w\\.~!$&'()*+,;=@-]|%[0-9A-Fa-f][0-9A-Fa-f])+";
    std::string path_noscheme = segment_nz_nc + "(/" + pchar + "*)*";
    userinfo = "([\\w\\.~!$&'()*+,;=:-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    host = "([\\w\\.~!$&'()*+,;=-]|%[0-9A-Fa-f][0-9A-Fa-f])*";
    authority = "(" + userinfo + "@)?" + host + "(:\\d*)?";
    std::string relative_part =
        "(//" + authority + path_abempty + "|" + path_absolute + "|" + path_noscheme + ")?";
    m["uri-reference"] = "^" + relative_part + query + fragment + "$";

    std::string literals =
        "([\\x21\\x23-\\x24\\x26\\x28-\\x3B\\x3D\\x3F-\\x5B\\x5D\\x5F\\x61-\\x7A\\x7E]"
        "|%[0-9A-Fa-f][0-9A-Fa-f])";
    std::string op = "[+#\\./;\\?&=,!@|]";
    std::string varchar = "(\\w|%[0-9A-Fa-f][0-9A-Fa-f])";
    std::string varname = varchar + "(\\.?" + varchar + ")*";
    std::string varspec = varname + "(:[1-9]\\d?\\d?\\d?|\\*)?";
    std::string variable_list = varspec + "(," + varspec + ")*";
    std::string expression = "\\{(" + op + ")?" + variable_list + "\\}";
    m["uri-template"] = "^(" + literals + "|" + expression + ")*$";

    m["json-pointer"] = "^(/([\\x00-\\x2E]|[\\x30-\\x7D]|[\\x7F-\\U0010FFFF]|~[01])*)*$";
    m["relative-json-pointer"] =
        "^(0|[1-9][0-9]*)(#|(/([\\x00-\\x2E]|[\\x30-\\x7D]|[\\x7F-\\U0010FFFF]|~[01])*)*)$";

    return m;
  }();

  auto it = regex_map.find(format);
  if (it == regex_map.end()) {
    return std::nullopt;
  }
  return it->second;
}

// ==================== Range Regex Generation ====================

// Stateless utility that turns a numeric range into an anchored regex matching
// exactly the JSON integers / numbers inside it. Every method is static; the
// class exists only to group the helpers and keep the internal ones private.
class NumberGenerator {
 public:
  // Anchored regex matching every integer x with start <= x <= end. Either bound
  // may be std::nullopt for an open side; an empty range yields "^()$". Bounds
  // span the whole int64 range (|INT64_MIN| is handled without negation overflow).
  static std::string IntegerRangeRegex(std::optional<int64_t> start, std::optional<int64_t> end);
  static std::string IntegerRangeRegexStrings(
      const std::optional<std::string>& start, const std::optional<std::string>& end
  );

  // Anchored regex matching every number in the range, written with up to
  // `precision` fraction digits. `exclusive_start` / `exclusive_end` exclude the
  // boundary value itself (turning >= / <= into > / <). Either bound may be
  // std::nullopt for an open side; an empty range yields "^()$".
  static std::string FloatRangeRegex(
      std::optional<double> start,
      std::optional<double> end,
      int precision,
      bool exclusive_start,
      bool exclusive_end
  );

 private:
  // Regex alternatives for the fraction digits following a decimal point.
  struct FracPatternSet {
    // Each pattern matches a non-empty fraction digit string.
    std::vector<std::string> parts;
    // Whether having no fraction digits at all also satisfies the bound.
    bool include_empty = false;
  };

  // --- Regex fragment primitives ---
  static std::string DigitClass(char lo, char hi);  // one digit in [lo, hi] (or \d)
  static std::string ExactDigits(int k);            // exactly k free digits: \d{k}
  static std::string FreeDigits(int max_count);     // 0..max_count free digits: \d{0,n}
  static std::string OptionalZeros(int max_count);  // 0..max_count zeros: 0{0,n}
  static std::string SomeZeros(int max_count);      // 1..max_count zeros: 0{1,n}
  static bool AllChar(const std::string& s, char c);

  // --- Integer range (operate on non-negative decimal magnitude strings) ---
  static std::string AbsDigits(int64_t v);
  static int CompareDigitStr(const std::string& a, const std::string& b);
  static std::vector<std::string> IntSameLen(const std::string& a, const std::string& b);
  static std::vector<std::string> NumberPatternsStr(const std::string& lo, const std::string& hi);
  static std::string SubRangeRegexStr(const std::string& lo, const std::string& hi);
  static std::vector<std::string> AtLeastPositivePatternsStr(const std::string& v_str);

  // --- Float range ---
  static std::string FormatFloat(double value, int precision);
  // Snaps a non-negative bound to the precision grid in the direction that keeps
  // the range sound: a lower bound rounds up, an upper bound rounds down, so no
  // out-of-range value is ever admitted. Returns the canonical grid string and,
  // via strict_out, whether the boundary value must still be excluded.
  static std::string RoundBoundToGrid(
      double value, int precision, bool is_lower, bool strict_in, bool* strict_out
  );
  // Adds (inc) or subtracts (!inc) one grid step (10^-precision) to a canonical
  // non-negative decimal string, returning the canonical result.
  static std::string AdjustGrid(const std::string& s, int precision, bool inc);
  static void SplitDecimal(const std::string& s, std::string* int_part, std::string* frac_part);
  static int CompareDecimal(
      const std::string& int_a,
      const std::string& frac_a,
      const std::string& int_b,
      const std::string& frac_b
  );
  static std::string StripAnchors(const std::string& regex);
  static int64_t ParseIntCapped(const std::string& digits);
  static FracPatternSet FracGreaterPatterns(const std::string& s, bool strict, int max_len);
  static FracPatternSet FracLessPatterns(const std::string& s, bool strict, int max_len);
  static FracPatternSet FracBetweenPatterns(
      const std::string& a, bool strict_a, const std::string& b, bool strict_b, int max_len
  );
  static std::vector<std::string> PositiveRangeParts(
      const std::string& low,
      bool strict_low,
      const std::optional<std::string>& high,
      bool strict_high,
      int precision
  );
};

// Helpers for integer range regex generation. They operate purely on
// fixed-length decimal digit strings (suffixes may carry leading zeros), so the
// patterns are correct by construction regardless of digit position.

// A regex fragment matching a single digit in [lo, hi].
std::string NumberGenerator::DigitClass(char lo, char hi) {
  if (lo == hi) {
    return std::string(1, lo);
  }
  if (lo == '0' && hi == '9') {
    return "\\d";
  }
  return "[" + std::string(1, lo) + "-" + std::string(1, hi) + "]";
}

// A regex fragment matching k free digits (each 0-9). Empty when k <= 0.
std::string NumberGenerator::ExactDigits(int k) {
  if (k <= 0) {
    return "";
  }
  if (k == 1) {
    return "\\d";
  }
  return "\\d{" + std::to_string(k) + "}";
}

bool NumberGenerator::AllChar(const std::string& s, char c) {
  return std::all_of(s.begin(), s.end(), [c](char ch) { return ch == c; });
}

// Patterns matching every equal-length digit string t with
// value(a) <= value(t) <= value(b). Requires a.size() == b.size() and
// value(a) <= value(b). Partitions t by its first digit:
//   * first digit == a[0]: the suffix must be >= a's suffix (<= 99..9);
//   * first digit strictly between a[0] and b[0]: the suffix is unconstrained;
//   * first digit == b[0]: the suffix must be <= b's suffix (>= 00..0).
// The partition is exact and non-overlapping, so the union is sound and
// complete for [a, b].
std::vector<std::string> NumberGenerator::IntSameLen(const std::string& a, const std::string& b) {
  int n = static_cast<int>(a.size());
  if (a == b) {
    return {a};
  }
  if (n == 1) {
    return {DigitClass(a[0], b[0])};
  }
  if (a[0] == b[0]) {
    std::vector<std::string> res;
    for (auto& p : IntSameLen(a.substr(1), b.substr(1))) {
      res.push_back(std::string(1, a[0]) + p);
    }
    return res;
  }
  // a[0] < b[0]
  std::string a_suf = a.substr(1);
  std::string b_suf = b.substr(1);
  if (AllChar(a_suf, '0') && AllChar(b_suf, '9')) {
    // The whole suffix space is free: collapse to one box pattern.
    if (a[0] == '0' && b[0] == '9') {
      return {ExactDigits(n)};
    }
    return {DigitClass(a[0], b[0]) + ExactDigits(n - 1)};
  }
  std::vector<std::string> res;
  std::string nines(n - 1, '9');
  std::string zeros(n - 1, '0');
  for (auto& p : IntSameLen(a_suf, nines)) {
    res.push_back(std::string(1, a[0]) + p);
  }
  if (b[0] - a[0] >= 2) {
    res.push_back(
        DigitClass(static_cast<char>(a[0] + 1), static_cast<char>(b[0] - 1)) + ExactDigits(n - 1)
    );
  }
  for (auto& p : IntSameLen(zeros, b_suf)) {
    res.push_back(std::string(1, b[0]) + p);
  }
  return res;
}

// Compares two non-negative decimal magnitude strings (no leading zeros except
// "0") by value.
int NumberGenerator::CompareDigitStr(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) {
    return a.size() < b.size() ? -1 : 1;
  }
  if (a < b) {
    return -1;
  }
  return a > b ? 1 : 0;
}

// Patterns matching every integer whose magnitude has value in [lo, hi], where
// lo and hi are non-negative decimal magnitude strings (no leading zeros except
// "0"). An empty range (value(lo) > value(hi)) yields no patterns. Operating on
// strings keeps the whole int64 range representable, including
// |INT64_MIN| = 9223372036854775808, which does not fit in int64.
std::vector<std::string> NumberGenerator::NumberPatternsStr(
    const std::string& lo, const std::string& hi
) {
  std::vector<std::string> patterns;
  if (CompareDigitStr(lo, hi) > 0) {
    return patterns;
  }
  int lo_len = static_cast<int>(lo.size());
  int hi_len = static_cast<int>(hi.size());
  // Split [lo, hi] by digit length; each length yields a same-length segment
  // handled exactly by IntSameLen.
  for (int len = lo_len; len <= hi_len; ++len) {
    std::string a_str = (len == lo_len) ? lo : ("1" + std::string(len - 1, '0'));
    std::string b_str = (len == hi_len) ? hi : std::string(len, '9');
    for (auto& p : IntSameLen(a_str, b_str)) {
      patterns.push_back(p);
    }
  }
  return patterns;
}

// Joins NumberPatternsStr alternatives into a parenthesised regex group.
std::string NumberGenerator::SubRangeRegexStr(const std::string& lo, const std::string& hi) {
  std::vector<std::string> patterns = NumberPatternsStr(lo, hi);
  std::string joined;
  for (size_t i = 0; i < patterns.size(); ++i) {
    if (i > 0) {
      joined += "|";
    }
    joined += patterns[i];
  }
  return "(" + joined + ")";
}

// Patterns matching every integer in [value(v_str), +infinity) for v_str a
// positive magnitude string (no leading zeros). Same-length values come from
// IntSameLen(v_str, 99..9); strictly longer values are any non-zero-led number.
std::vector<std::string> NumberGenerator::AtLeastPositivePatternsStr(const std::string& v_str) {
  int len = static_cast<int>(v_str.size());
  std::vector<std::string> res = IntSameLen(v_str, std::string(len, '9'));
  res.push_back("[1-9]\\d{" + std::to_string(len) + ",}");
  return res;
}

// The magnitude (absolute value) of v as a decimal string. Derived from the
// signed text rather than by negating v, so INT64_MIN is handled correctly.
std::string NumberGenerator::AbsDigits(int64_t v) {
  std::string s = std::to_string(v);
  return (!s.empty() && s[0] == '-') ? s.substr(1) : s;
}

std::string NumberGenerator::IntegerRangeRegex(
    std::optional<int64_t> start, std::optional<int64_t> end
) {
  return IntegerRangeRegexStrings(
      start.has_value() ? std::optional<std::string>(std::to_string(*start)) : std::nullopt,
      end.has_value() ? std::optional<std::string>(std::to_string(*end)) : std::nullopt
  );
}

std::string NumberGenerator::IntegerRangeRegexStrings(
    const std::optional<std::string>& start, const std::optional<std::string>& end
) {
  std::vector<std::string> parts;
  std::ostringstream result;

  if (!start && !end) {
    return "^-?\\d+$";
  }

  if (start && !end) {
    int start_sign = CompareCanonicalIntegers(*start, "0");
    if (start_sign <= 0) {
      if (start_sign < 0) {
        // Negatives in [start, -1] are the magnitudes [1, |start|], negated.
        parts.push_back("-" + SubRangeRegexStr("1", IntegerMagnitude(*start)));
      }
      parts.push_back("0");
      parts.push_back("[1-9]\\d*");
    } else {
      // x >= start with start > 0: same-length values >= start, plus every
      // value with strictly more digits.
      for (auto& p : AtLeastPositivePatternsStr(*start)) {
        parts.push_back(p);
      }
    }
  }

  if (!start && end) {
    int end_sign = CompareCanonicalIntegers(*end, "0");
    if (end_sign >= 0) {
      parts.push_back("-[1-9]\\d*");
      parts.push_back("0");
      if (end_sign > 0) {
        parts.push_back(SubRangeRegexStr("1", *end));
      }
    } else {
      // x <= end with end < 0: x = -a where a >= |end| > 0, so negate every
      // pattern for the range [|end|, +infinity).
      for (auto& p : AtLeastPositivePatternsStr(IntegerMagnitude(*end))) {
        parts.push_back("-" + p);
      }
    }
  }

  if (start && end) {
    if (CompareCanonicalIntegers(*start, *end) > 0) {
      return "^()$";
    }

    if (CompareCanonicalIntegers(*start, "0") < 0) {
      std::string neg_end = CompareCanonicalIntegers(*end, "-1") < 0 ? *end : std::string("-1");
      // Negatives in [neg_start, neg_end] are the magnitudes
      // [|neg_end|, |neg_start|], negated.
      parts.push_back("-" + SubRangeRegexStr(IntegerMagnitude(neg_end), IntegerMagnitude(*start)));
    }

    if (CompareCanonicalIntegers(*start, "0") <= 0 && CompareCanonicalIntegers(*end, "0") >= 0) {
      parts.push_back("0");
    }

    if (CompareCanonicalIntegers(*end, "0") > 0) {
      std::string pos_start = CompareCanonicalIntegers(*start, "1") > 0 ? *start : std::string("1");
      parts.push_back(SubRangeRegexStr(pos_start, *end));
    }
  }

  result << "^(";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      result << "|";
    }
    result << parts[i];
  }
  result << ")$";

  return result.str();
}

std::string NumberGenerator::FormatFloat(double value, int precision) {
  // Casting a double outside [INT64_MIN, INT64_MAX] (or NaN/Inf) to int64_t is
  // undefined behavior, so range-check before the integer fast path. 2^63 ==
  // 9223372036854775808.0 is exactly representable and one past INT64_MAX, so the
  // upper comparison must be strict.
  if (value >= -9223372036854775808.0 && value < 9223372036854775808.0 &&
      value == static_cast<int64_t>(value)) {
    return std::to_string(static_cast<int64_t>(value));
  }

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << value;
  std::string result = oss.str();

  size_t decimalPos = result.find('.');
  if (decimalPos != std::string::npos) {
    size_t lastNonZero = result.find_last_not_of('0');
    if (lastNonZero != std::string::npos && lastNonZero > decimalPos) {
      result.erase(lastNonZero + 1);
    } else if (lastNonZero == decimalPos) {
      result.erase(decimalPos);
    }
  }

  return result;
}

std::string NumberGenerator::AdjustGrid(const std::string& s, int precision, bool inc) {
  std::string int_part, frac_part;
  SplitDecimal(s, &int_part, &frac_part);
  // Build the scaled-integer numerator (value * 10^precision) as a digit string.
  // Callers only pass FormatFloat output (<= precision fraction digits); guard
  // the count so a longer string can never wrap the unsigned append count.
  frac_part.append(std::max(0, precision - static_cast<int>(frac_part.size())), '0');
  std::string num = int_part + frac_part;

  if (inc) {
    int i = static_cast<int>(num.size()) - 1;
    for (; i >= 0 && num[i] == '9'; --i) {
      num[i] = '0';
    }
    if (i < 0) {
      num.insert(num.begin(), '1');
    } else {
      num[i]++;
    }
  } else {
    int i = static_cast<int>(num.size()) - 1;
    for (; i >= 0 && num[i] == '0'; --i) {
      num[i] = '9';
    }
    if (i < 0) {
      // Underflow below zero; clamp to zero (does not occur for the bounds the
      // float pipeline feeds in, which are all >= one grid step when decremented).
      num.assign(num.size(), '0');
    } else {
      num[i]--;
    }
  }

  // Re-split into integer and `precision`-digit fraction, then canonicalize.
  while (static_cast<int>(num.size()) <= precision) {
    num.insert(num.begin(), '0');
  }
  std::string new_int = num.substr(0, num.size() - precision);
  std::string new_frac = num.substr(num.size() - precision);
  size_t nz = new_int.find_first_not_of('0');
  new_int = (nz == std::string::npos) ? "0" : new_int.substr(nz);
  size_t lnz = new_frac.find_last_not_of('0');
  new_frac = (lnz == std::string::npos) ? "" : new_frac.substr(0, lnz + 1);
  return new_frac.empty() ? new_int : new_int + "." + new_frac;
}

std::string NumberGenerator::RoundBoundToGrid(
    double value, int precision, bool is_lower, bool strict_in, bool* strict_out
) {
  // FormatFloat rounds to the nearest grid point; if that lands exactly on the
  // bound, keep the original strictness. Otherwise step to the grid point just
  // inside the range so no out-of-range value is admitted, and the boundary is
  // now strictly interior, so it becomes inclusive.
  std::string r = FormatFloat(value, precision);
  double rv = std::stod(r);
  if (rv == value) {
    *strict_out = strict_in;
    return r;
  }
  *strict_out = false;
  if (is_lower && rv < value) {
    // Rounded below a lower bound: move up to the smallest grid point >= value.
    r = AdjustGrid(r, precision, /*inc=*/true);
  } else if (!is_lower && rv > value) {
    // Rounded above an upper bound: move down to the largest grid point <= value.
    r = AdjustGrid(r, precision, /*inc=*/false);
  }
  return r;
}

// Helpers for GenerateFloatRangeRegex. Fraction patterns operate on the
// digit string after the decimal point, compared against a canonical bound
// fraction (canonical: produced by FormatFloat, so no trailing zeros).

// Matches 0 to max_count free digits.
std::string NumberGenerator::FreeDigits(int max_count) {
  if (max_count <= 0) {
    return "";
  }
  return "\\d{0," + std::to_string(max_count) + "}";
}

// Matches 0 to max_count zeros.
std::string NumberGenerator::OptionalZeros(int max_count) {
  if (max_count <= 0) {
    return "";
  }
  return "0{0," + std::to_string(max_count) + "}";
}

// Matches 1 to max_count zeros.
std::string NumberGenerator::SomeZeros(int max_count) {
  return "0{1," + std::to_string(max_count) + "}";
}

// Patterns for fraction strings t (1 <= |t| <= max_len) whose value 0.t is
// greater than 0.s (or equal when !strict). |s| <= max_len.
NumberGenerator::FracPatternSet NumberGenerator::FracGreaterPatterns(
    const std::string& s, bool strict, int max_len
) {
  FracPatternSet result;
  int n = static_cast<int>(s.size());
  // t agrees with s up to position i, then has a larger digit
  for (int i = 0; i < n; ++i) {
    if (s[i] < '9') {
      result.parts.push_back(
          s.substr(0, i) + DigitClass(s[i] + 1, '9') + FreeDigits(max_len - i - 1)
      );
    }
  }
  // t extends s with a nonzero digit (after optional zeros)
  for (int k = 0; n + k + 1 <= max_len; ++k) {
    result.parts.push_back(s + std::string(k, '0') + "[1-9]" + FreeDigits(max_len - n - k - 1));
  }
  if (!strict) {
    // t has the same value as s: s plus optional trailing zeros
    if (n > 0) {
      result.parts.push_back(s + OptionalZeros(max_len - n));
    } else {
      result.include_empty = true;
      if (max_len >= 1) {
        result.parts.push_back(SomeZeros(max_len));
      }
    }
  }
  return result;
}

// Patterns for fraction strings t (1 <= |t| <= max_len) whose value 0.t is
// less than 0.s (or equal when !strict). |s| <= max_len.
NumberGenerator::FracPatternSet NumberGenerator::FracLessPatterns(
    const std::string& s, bool strict, int max_len
) {
  FracPatternSet result;
  int n = static_cast<int>(s.size());
  // t agrees with s up to position i, then has a smaller digit
  for (int i = 0; i < n; ++i) {
    if (s[i] > '0') {
      result.parts.push_back(
          s.substr(0, i) + DigitClass('0', s[i] - 1) + FreeDigits(max_len - i - 1)
      );
    }
  }
  // t is a proper prefix of s plus optional trailing zeros: strictly smaller,
  // since the remaining digits of s contain a nonzero one
  for (int i = 0; i < n; ++i) {
    if (i == 0) {
      if (max_len >= 1) {
        result.parts.push_back(SomeZeros(max_len));
      }
    } else {
      result.parts.push_back(s.substr(0, i) + OptionalZeros(max_len - i));
    }
  }
  if (!strict) {
    // t has the same value as s
    if (n > 0) {
      result.parts.push_back(s + OptionalZeros(max_len - n));
    } else if (max_len >= 1) {
      result.parts.push_back(SomeZeros(max_len));
    }
  }
  result.include_empty = n > 0 || !strict;
  return result;
}

// Patterns for fraction strings t whose value 0.t lies between 0.a and 0.b.
// Requires value(0.a) < value(0.b) and b non-empty.
NumberGenerator::FracPatternSet NumberGenerator::FracBetweenPatterns(
    const std::string& a, bool strict_a, const std::string& b, bool strict_b, int max_len
) {
  FracPatternSet result;
  // Longest common prefix of b and zero-padded a. Always stops before |b|:
  // value(0.a) < value(0.b) implies b is not a prefix of padded a.
  int common_len = 0;
  while (common_len < static_cast<int>(b.size()) &&
         (common_len < static_cast<int>(a.size()) ? a[common_len] : '0') == b[common_len]) {
    ++common_len;
  }
  std::string common = b.substr(0, common_len);
  char digit_a = common_len < static_cast<int>(a.size()) ? a[common_len] : '0';
  char digit_b = b[common_len];

  // a digit strictly between the bounds' digits, then anything
  if (digit_b - digit_a >= 2) {
    result.parts.push_back(
        common + DigitClass(digit_a + 1, digit_b - 1) + FreeDigits(max_len - common_len - 1)
    );
  }
  // lower boundary: t continues with digit_a, the rest must exceed a's suffix
  if (common_len < static_cast<int>(a.size())) {
    FracPatternSet sub_lower =
        FracGreaterPatterns(a.substr(common_len + 1), strict_a, max_len - common_len - 1);
    for (auto& part : sub_lower.parts) {
      result.parts.push_back(common + digit_a + std::move(part));
    }
    if (sub_lower.include_empty) {
      result.parts.push_back(common + std::string(1, digit_a));
    }
  } else {
    // a's value equals value(0.common): only nonzero extensions of
    // common + digit_a ('0') are strictly greater
    FracPatternSet sub_lower = FracGreaterPatterns("", true, max_len - common_len - 1);
    for (auto& part : sub_lower.parts) {
      result.parts.push_back(common + digit_a + std::move(part));
    }
    if (!strict_a) {
      // t has the same value as a
      if (!a.empty()) {
        result.parts.push_back(a + OptionalZeros(max_len - static_cast<int>(a.size())));
      } else {
        result.include_empty = true;
        if (max_len >= 1) {
          result.parts.push_back(SomeZeros(max_len));
        }
      }
    }
  }
  // upper boundary: t continues with digit_b, the rest must stay below b's suffix
  FracPatternSet sub_upper =
      FracLessPatterns(b.substr(common_len + 1), strict_b, max_len - common_len - 1);
  for (auto& part : sub_upper.parts) {
    result.parts.push_back(common + digit_b + std::move(part));
  }
  if (sub_upper.include_empty) {
    result.parts.push_back(common + std::string(1, digit_b));
  }
  return result;
}

// Splits a canonical decimal string from FormatFloat ("12" or "12.34") into
// integer and fraction parts.
void NumberGenerator::SplitDecimal(
    const std::string& s, std::string* int_part, std::string* frac_part
) {
  size_t dot = s.find('.');
  if (dot == std::string::npos) {
    *int_part = s;
    frac_part->clear();
  } else {
    *int_part = s.substr(0, dot);
    *frac_part = s.substr(dot + 1);
  }
}

// Compares the values of two canonical non-negative decimals.
int NumberGenerator::CompareDecimal(
    const std::string& int_a,
    const std::string& frac_a,
    const std::string& int_b,
    const std::string& frac_b
) {
  if (int_a.size() != int_b.size()) {
    return int_a.size() < int_b.size() ? -1 : 1;
  }
  if (int_a != int_b) {
    return int_a < int_b ? -1 : 1;
  }
  size_t max_frac = std::max(frac_a.size(), frac_b.size());
  for (size_t i = 0; i < max_frac; ++i) {
    char da = i < frac_a.size() ? frac_a[i] : '0';
    char db = i < frac_b.size() ? frac_b[i] : '0';
    if (da != db) {
      return da < db ? -1 : 1;
    }
  }
  return 0;
}

// Strips the ^( )$ anchors added by IntegerRangeRegex, keeping the group.
std::string NumberGenerator::StripAnchors(const std::string& regex) {
  return regex.substr(1, regex.size() - 2);
}

int64_t NumberGenerator::ParseIntCapped(const std::string& digits) {
  // `digits` is a canonical non-negative integer string (no leading zeros).
  // Parse it exactly when it fits in int64; clamp to INT64_MAX otherwise (such
  // magnitudes are beyond practical float bounds and double integer precision).
  static const std::string kMaxInt64 = std::to_string(std::numeric_limits<int64_t>::max());
  if (digits.size() > kMaxInt64.size() ||
      (digits.size() == kMaxInt64.size() && digits > kMaxInt64)) {
    return std::numeric_limits<int64_t>::max();
  }
  return std::stoll(digits);
}

// Patterns for unsigned decimals (integer part plus optional fraction of up
// to `precision` digits) within the given bounds. `low` is required and
// non-negative; `high` is optional. Patterns for the value 0 are never
// produced: when low's value is 0 the bound is treated as strict, and the
// caller emits the zero pattern itself.
std::vector<std::string> NumberGenerator::PositiveRangeParts(
    const std::string& low,
    bool strict_low,
    const std::optional<std::string>& high,
    bool strict_high,
    int precision
) {
  std::vector<std::string> parts;
  std::string int_low, frac_low;
  SplitDecimal(low, &int_low, &frac_low);
  if (int_low == "0" && frac_low.empty()) {
    strict_low = true;
  }
  int64_t int_low_value = ParseIntCapped(int_low);
  std::string opt_any_frac = "(\\.\\d{1," + std::to_string(precision) + "})?";

  auto add_with_int_part = [&](const std::string& int_part, const FracPatternSet& set) {
    for (const auto& part : set.parts) {
      parts.push_back(int_part + "\\." + part);
    }
    if (set.include_empty) {
      parts.push_back(int_part);
    }
  };

  if (!high.has_value()) {
    add_with_int_part(int_low, FracGreaterPatterns(frac_low, strict_low, precision));
    // Guard the +1 against int64 overflow (int_low_value may be clamped to
    // INT64_MAX for very large bounds).
    if (int_low_value < std::numeric_limits<int64_t>::max()) {
      parts.push_back(
          StripAnchors(IntegerRangeRegex(int_low_value + 1, std::nullopt)) + opt_any_frac
      );
    }
    return parts;
  }

  std::string int_high, frac_high;
  SplitDecimal(*high, &int_high, &frac_high);
  int64_t int_high_value = ParseIntCapped(int_high);
  int cmp = CompareDecimal(int_low, frac_low, int_high, frac_high);
  if (cmp > 0 || (cmp == 0 && (strict_low || strict_high))) {
    return parts;
  }
  if (cmp == 0) {
    // single representable value, with optional redundant trailing zeros
    if (frac_low.empty()) {
      parts.push_back(int_low + "(\\." + SomeZeros(precision) + ")?");
    } else {
      parts.push_back(
          int_low + "\\." + frac_low + OptionalZeros(precision - static_cast<int>(frac_low.size()))
      );
    }
    return parts;
  }
  if (int_low == int_high) {
    add_with_int_part(
        int_low, FracBetweenPatterns(frac_low, strict_low, frac_high, strict_high, precision)
    );
  } else {
    add_with_int_part(int_low, FracGreaterPatterns(frac_low, strict_low, precision));
    if (int_high_value - int_low_value >= 2) {
      parts.push_back(
          StripAnchors(IntegerRangeRegex(int_low_value + 1, int_high_value - 1)) + opt_any_frac
      );
    }
    add_with_int_part(int_high, FracLessPatterns(frac_high, strict_high, precision));
  }
  return parts;
}

std::string NumberGenerator::FloatRangeRegex(
    std::optional<double> start,
    std::optional<double> end,
    int precision,
    bool exclusive_start,
    bool exclusive_end
) {
  if (start && end) {
    if (start.value() > end.value() ||
        (start.value() == end.value() && (exclusive_start || exclusive_end))) {
      return "^()$";
    }
  }

  if (!start && !end) {
    return "^-?\\d+(\\.\\d{1," + std::to_string(precision) + "})?$";
  }

  std::vector<std::string> parts;

  // Negative values: x is in [start, end] iff -x is in [-end, -start], so the
  // positive-range patterns are reused on the negated bounds and prefixed
  // with '-'.
  bool negatives_in_range = !start.has_value() || start.value() < 0;
  if (negatives_in_range) {
    std::string low = "0";
    bool strict_low = true;
    if (end.has_value() && end.value() < 0) {
      low =
          RoundBoundToGrid(-end.value(), precision, /*is_lower=*/true, exclusive_end, &strict_low);
    }
    std::optional<std::string> high;
    bool strict_high = false;
    if (start.has_value()) {
      high = RoundBoundToGrid(
          -start.value(), precision, /*is_lower=*/false, exclusive_start, &strict_high
      );
    }
    for (auto& part : PositiveRangeParts(low, strict_low, high, strict_high, precision)) {
      parts.push_back("-" + std::move(part));
    }
  }

  bool zero_allowed =
      (!start.has_value() || start.value() < 0 || (start.value() == 0 && !exclusive_start)) &&
      (!end.has_value() || end.value() > 0 || (end.value() == 0 && !exclusive_end));
  if (zero_allowed) {
    parts.push_back("0(\\." + SomeZeros(precision) + ")?");
    // Negative zero written with an all-zero fraction ("-0.0".."-0.000000") also
    // denotes 0. PositiveRangeParts never emits magnitude 0, so add these forms
    // explicitly when the range covers the negative side.
    if (negatives_in_range) {
      parts.push_back("-0(\\." + SomeZeros(precision) + ")");
    }
  }

  // Positive values
  if (!end.has_value() || end.value() > 0) {
    std::string low = "0";
    bool strict_low = true;
    if (start.has_value() && start.value() > 0) {
      low = RoundBoundToGrid(
          start.value(), precision, /*is_lower=*/true, exclusive_start, &strict_low
      );
    }
    std::optional<std::string> high;
    bool strict_high = false;
    if (end.has_value()) {
      high =
          RoundBoundToGrid(end.value(), precision, /*is_lower=*/false, exclusive_end, &strict_high);
    }
    for (auto& part : PositiveRangeParts(low, strict_low, high, strict_high, precision)) {
      parts.push_back(std::move(part));
    }
  }

  std::ostringstream result;
  result << "^(";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) {
      result << "|";
    }
    result << parts[i];
  }
  result << ")$";

  return result.str();
}

std::string JSONSchemaConverter::GenerateRangeRegex(
    std::optional<int64_t> start, std::optional<int64_t> end
) {
  return NumberGenerator::IntegerRangeRegex(start, end);
}

std::string JSONSchemaConverter::GenerateRangeRegex(
    const std::optional<std::string>& start, const std::optional<std::string>& end
) {
  return NumberGenerator::IntegerRangeRegexStrings(start, end);
}

std::string JSONSchemaConverter::GenerateFloatRangeRegex(
    std::optional<double> start,
    std::optional<double> end,
    int precision,
    bool exclusive_start,
    bool exclusive_end
) {
  return NumberGenerator::FloatRangeRegex(start, end, precision, exclusive_start, exclusive_end);
}

// ==================== Public API Functions ====================

std::optional<JSONFormat> JSONFormatFromString(const std::string& format) {
  static const std::unordered_map<std::string, JSONFormat> kNameToFormat = {
      {"json", JSONFormat::kJSON},
      {"qwen_xml", JSONFormat::kQwenXML},
      {"minimax_xml", JSONFormat::kMiniMaxXML},
      {"deepseek_xml", JSONFormat::kDeepSeekXML},
      {"glm_xml", JSONFormat::kGlmXML},
  };
  auto it = kNameToFormat.find(format);
  if (it == kNameToFormat.end()) {
    return std::nullopt;
  }
  return it->second;
}

Grammar JSONSchemaToGrammar(
    const std::string& schema,
    bool any_whitespace,
    std::optional<int> indent,
    std::optional<std::pair<std::string, std::string>> separators,
    bool strict_mode,
    std::optional<int> max_whitespace_cnt,
    bool any_order,
    JSONFormat json_format,
    RegexFSMCache* regex_fsm_cache
) {
  picojson::value schema_value;
  std::string preserved_schema = PreserveExactNumericConstraints(schema);
  std::string error = picojson::parse(schema_value, preserved_schema);
  XGRAMMAR_CHECK(error.empty()) << "Failed to parse JSON: " << error
                                << ". The JSON string is:" << schema;
  SchemaParser parser(schema_value, {strict_mode, json_format});
  auto spec_result = parser.Parse(schema_value, "root");
  if (spec_result.IsErr()) {
    XGRAMMAR_LOG(FATAL) << std::move(spec_result).UnwrapErr().what();
  }
  auto spec = std::move(spec_result).Unwrap();
  auto ref_resolver = [&parser](const std::string& uri, const std::string& rule_name_hint) {
    auto result = parser.ResolveRef(uri, rule_name_hint);
    if (result.IsErr()) {
      XGRAMMAR_LOG(FATAL) << std::move(result).UnwrapErr().what();
    }
    return std::move(result).Unwrap();
  };

  switch (json_format) {
    case JSONFormat::kJSON: {
      JSONSchemaConverter converter(
          indent,
          std::move(separators),
          any_whitespace,
          max_whitespace_cnt,
          std::move(ref_resolver),
          any_order,
          regex_fsm_cache
      );
      return converter.Convert(spec);
    }
    case JSONFormat::kQwenXML:
    case JSONFormat::kMiniMaxXML:
    case JSONFormat::kDeepSeekXML:
    case JSONFormat::kGlmXML: {
      XMLToolCallingConverter converter(
          indent,
          std::move(separators),
          any_whitespace,
          max_whitespace_cnt,
          std::move(ref_resolver),
          json_format,
          any_order,
          regex_fsm_cache
      );
      return converter.Convert(spec);
    }
    default:
      XGRAMMAR_LOG(FATAL) << "Invalid JSON format: " << static_cast<int>(json_format);
  }
  XGRAMMAR_UNREACHABLE();
}

std::string JSONSchemaToEBNF(
    const std::string& schema,
    bool any_whitespace,
    std::optional<int> indent,
    std::optional<std::pair<std::string, std::string>> separators,
    bool strict_mode,
    std::optional<int> max_whitespace_cnt,
    JSONFormat json_format,
    bool any_order
) {
  picojson::value schema_value;
  std::string preserved_schema = PreserveExactNumericConstraints(schema);
  std::string err = picojson::parse(schema_value, preserved_schema);
  XGRAMMAR_CHECK(err.empty()) << "Failed to parse JSON: " << err
                              << ". The JSON string is:" << schema;
  return JSONSchemaToEBNF(
      schema_value,
      any_whitespace,
      indent,
      separators,
      strict_mode,
      max_whitespace_cnt,
      json_format,
      any_order
  );
}

std::string JSONSchemaToEBNF(
    const picojson::value& schema,
    bool any_whitespace,
    std::optional<int> indent,
    std::optional<std::pair<std::string, std::string>> separators,
    bool strict_mode,
    std::optional<int> max_whitespace_cnt,
    JSONFormat json_format,
    bool any_order
) {
  // Parse JSON Schema to SchemaSpec
  SchemaParser parser(schema, {strict_mode, json_format});
  auto spec_result = parser.Parse(schema, "root");
  if (spec_result.IsErr()) {
    XGRAMMAR_LOG(FATAL) << std::move(spec_result).UnwrapErr().what();
  }
  auto spec = std::move(spec_result).Unwrap();

  auto ref_resolver = [&parser](const std::string& uri, const std::string& rule_name_hint) {
    auto r = parser.ResolveRef(uri, rule_name_hint);
    if (r.IsErr()) {
      XGRAMMAR_LOG(FATAL) << std::move(r).UnwrapErr().what();
    }
    return std::move(r).Unwrap();
  };

  // Create converter based on format
  switch (json_format) {
    case JSONFormat::kJSON: {
      JSONSchemaConverter converter(
          indent,
          separators,
          any_whitespace,
          max_whitespace_cnt,
          ref_resolver,
          any_order,
          nullptr,
          /*enable_runtime_json_string_constraints=*/false
      );
      return GrammarNormalizer::Apply(converter.Convert(spec)).ToString();
    }
    case JSONFormat::kQwenXML:
    case JSONFormat::kMiniMaxXML:
    case JSONFormat::kDeepSeekXML:
    case JSONFormat::kGlmXML: {
      XMLToolCallingConverter converter(
          indent,
          separators,
          any_whitespace,
          max_whitespace_cnt,
          ref_resolver,
          json_format,
          any_order,
          nullptr,
          /*enable_runtime_json_string_constraints=*/false
      );
      return GrammarNormalizer::Apply(converter.Convert(spec)).ToString();
    }
    default:
      XGRAMMAR_LOG(FATAL) << "Invalid JSON format: " << static_cast<int>(json_format);
  }
  XGRAMMAR_UNREACHABLE();
}

// Wrapper functions for testing
std::string GenerateRangeRegex(std::optional<int64_t> start, std::optional<int64_t> end) {
  return JSONSchemaConverter::GenerateRangeRegex(start, end);
}

std::string GenerateFloatRangeRegex(
    std::optional<double> start, std::optional<double> end, bool exclusive_start, bool exclusive_end
) {
  return JSONSchemaConverter::GenerateFloatRangeRegex(
      start, end, 6, exclusive_start, exclusive_end
  );
}

}  // namespace xgrammar
