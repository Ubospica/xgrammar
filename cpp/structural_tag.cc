/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/structural_tag.cc
 */
#include <picojson.h>
#include <xgrammar/structural_tag.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>

#include "grammar_functor.h"
#include "structural_tag_impl.h"
#include "support/logging.h"
#include "support/recursion_guard.h"

namespace xgrammar {

/************** StructuralTag Parser **************/

class StructuralTagImpl {
 public:
  static Result<StructuralTag> FromJSON(const std::string& json);

 private:
  Result<StructuralTag> ParseStructuralTag(const picojson::value& value);

  /*!
   * \brief Parse a Format object from a JSON value.
   * \param value The JSON value to parse.
   * \return A Format object if the JSON is valid, otherwise an error message in std::runtime_error.
   * \note The "type" field is checked in this function, and not checked in the Parse*Format
   * functions.
   */
  Result<Format> ParseFormat(const picojson::value& value);
  Result<LiteralFormat> ParseLiteralFormat(const picojson::object& value);
  Result<JSONSchemaFormat> ParseJSONSchemaFormat(const picojson::object& value);
  Result<WildcardTextFormat> ParseWildcardTextFormat(const picojson::object& value);
  Result<SequenceFormat> ParseSequenceFormat(const picojson::object& value);
  Result<OrFormat> ParseOrFormat(const picojson::object& value);
  /*! \brief ParseTagFormat with extra check for object and the type field. */
  Result<TagFormat> ParseTagFormat(const picojson::value& value);
  Result<TagFormat> ParseTagFormat(const picojson::object& value);
  Result<TriggeredTagsFormat> ParseTriggeredTagsFormat(const picojson::object& value);
  Result<TagsWithSeparatorFormat> ParseTagsWithSeparatorFormat(const picojson::object& value);

  int parse_format_recursion_depth_ = 0;
};

Result<StructuralTag> StructuralTagImpl::FromJSON(const std::string& json) {
  picojson::value value;
  std::string err = picojson::parse(value, json);
  if (!err.empty()) {
    return ResultErr("Failed to parse JSON: " + err);
  }
  return StructuralTagImpl().ParseStructuralTag(value);
}

Result<StructuralTag> StructuralTagImpl::ParseStructuralTag(const picojson::value& value) {
  if (!value.is<picojson::object>()) {
    return ResultErr("Structural tag must be an object");
  }
  const auto& obj = value.get<picojson::object>();
  // The type field is optional but must be "structural_tag" if present.
  if (obj.find("type") != obj.end()) {
    if (!obj["type"].is<std::string>() || obj["type"].get<std::string>() != "structural_tag") {
      return ResultErr("Structural tag's type must be a string \"structural_tag\"");
    }
  }
  // The format field is required.
  if (obj.find("format") == obj.end()) {
    return ResultErr("Structural tag must have a format field");
  }
  auto format = ParseFormat(obj["format"]);
  if (format.IsErr()) {
    return ResultErr(std::move(format).UnwrapErr());
  }
  return ResultOk(StructuralTag(std::move(format).Unwrap()));
}

Result<Format> StructuralTagImpl::ParseFormat(const picojson::value& value) {
  RecursionGuard guard(&parse_format_recursion_depth_);
  if (!value.is<picojson::object>()) {
    return ResultErr("Format must be an object");
  }
  const auto& obj = value.get<picojson::object>();
  // If type is present, use it to determine the format.
  if (obj.find("type") != obj.end()) {
    if (!obj["type"].is<std::string>()) {
      return ResultErr("Format's type must be a string");
    }
    auto type = obj["type"].get<std::string>();
    if (type == "literal") {
      return Result<Format>::Convert(ParseLiteralFormat(obj));
    } else if (type == "json_schema") {
      return Result<Format>::Convert(ParseJSONSchemaFormat(obj));
    } else if (type == "wildcard_text") {
      return Result<Format>::Convert(ParseWildcardTextFormat(obj));
    } else if (type == "sequence") {
      return Result<Format>::Convert(ParseSequenceFormat(obj));
    } else if (type == "or") {
      return Result<Format>::Convert(ParseOrFormat(obj));
    } else if (type == "tag") {
      return Result<Format>::Convert(ParseTagFormat(obj));
    } else if (type == "triggered_tags") {
      return Result<Format>::Convert(ParseTriggeredTagsFormat(obj));
    } else if (type == "tags_with_separator") {
      return Result<Format>::Convert(ParseTagsWithSeparatorFormat(obj));
    } else {
      return ResultErr("Invalid format type: " + type);
    }
  }

  // If type is not present, try every format type one by one. Tag is prioritized.
  auto tag_format = ParseTagFormat(obj);
  if (!tag_format.IsErr()) {
    return ResultOk<Format>(std::move(tag_format).Unwrap());
    // return Result<Format>::Ok(std::move(tag_format).Unwrap());
  }
  auto literal_format = ParseLiteralFormat(obj);
  if (!literal_format.IsErr()) {
    return ResultOk<Format>(std::move(literal_format).Unwrap());
  }
  auto json_schema_format = ParseJSONSchemaFormat(obj);
  if (!json_schema_format.IsErr()) {
    return ResultOk<Format>(std::move(json_schema_format).Unwrap());
  }
  auto wildcard_text_format = ParseWildcardTextFormat(obj);
  if (!wildcard_text_format.IsErr()) {
    return ResultOk<Format>(std::move(wildcard_text_format).Unwrap());
  }
  auto sequence_format = ParseSequenceFormat(obj);
  if (!sequence_format.IsErr()) {
    return ResultOk<Format>(std::move(sequence_format).Unwrap());
  }
  auto or_format = ParseOrFormat(obj);
  if (!or_format.IsErr()) {
    return ResultOk<Format>(std::move(or_format).Unwrap());
  }
  auto triggered_tags_format = ParseTriggeredTagsFormat(obj);
  if (!triggered_tags_format.IsErr()) {
    return ResultOk<Format>(std::move(triggered_tags_format).Unwrap());
  }
  auto tags_with_separator_format = ParseTagsWithSeparatorFormat(obj);
  if (!tags_with_separator_format.IsErr()) {
    return ResultOk<Format>(std::move(tags_with_separator_format).Unwrap());
  }
  return ResultErr("Invalid format: " + value.serialize(false));
}

Result<LiteralFormat> StructuralTagImpl::ParseLiteralFormat(const picojson::object& obj) {
  // text is required.
  auto text_it = obj.find("text");
  if (text_it == obj.end() || !text_it->second.is<std::string>() ||
      text_it->second.get<std::string>().empty()) {
    return ResultErr("Literal format must have a text field with a non-empty string");
  }
  return ResultOk<LiteralFormat>(text_it->second.get<std::string>());
}

Result<JSONSchemaFormat> StructuralTagImpl::ParseJSONSchemaFormat(const picojson::object& obj) {
  // json_schema is required.
  auto json_schema_it = obj.find("json_schema");
  if (json_schema_it == obj.end() || !json_schema_it->second.is<picojson::object>()) {
    return ResultErr("JSON schema format must have a json_schema field with a JSON object");
  }
  // here introduces a serialization/deserialization overhead; try to avoid it in the future.
  return ResultOk<JSONSchemaFormat>(json_schema_it->second.serialize(false));
}

Result<WildcardTextFormat> StructuralTagImpl::ParseWildcardTextFormat(const picojson::object& obj) {
  // obj should not have any fields other than "type"
  if (obj.size() > 1 || (obj.size() == 1 && obj.begin()->first != "type")) {
    return ResultErr("Wildcard text format should not have any fields other than type");
  }
  return ResultOk<WildcardTextFormat>();
}

Result<SequenceFormat> StructuralTagImpl::ParseSequenceFormat(const picojson::object& obj) {
  // elements is required.
  auto elements_it = obj.find("elements");
  if (elements_it == obj.end() || !elements_it->second.is<picojson::array>()) {
    return ResultErr("Sequence format must have an elements field with an array");
  }
  const auto& elements_array = elements_it->second.get<picojson::array>();
  std::vector<Format> elements;
  elements.reserve(elements_array.size());
  for (const auto& element : elements_array) {
    auto format = ParseFormat(element);
    if (format.IsErr()) {
      return ResultErr(std::move(format).UnwrapErr());
    }
    elements.push_back(std::move(format).Unwrap());
  }
  return ResultOk<SequenceFormat>(std::move(elements));
}

Result<OrFormat> StructuralTagImpl::ParseOrFormat(const picojson::object& obj) {
  // elements is required.
  auto elements_it = obj.find("elements");
  if (elements_it == obj.end() || !elements_it->second.is<picojson::array>()) {
    return ResultErr("Or format must have an elements field with an array");
  }
  const auto& elements_array = elements_it->second.get<picojson::array>();
  std::vector<Format> elements;
  elements.reserve(elements_array.size());
  for (const auto& element : elements_array) {
    auto format = ParseFormat(element);
    if (format.IsErr()) {
      return ResultErr(std::move(format).UnwrapErr());
    }
    elements.push_back(std::move(format).Unwrap());
  }
  return ResultOk<OrFormat>(std::move(elements));
}

Result<TagFormat> StructuralTagImpl::ParseTagFormat(const picojson::value& value) {
  if (!value.is<picojson::object>()) {
    return ResultErr("Tag format must be an object");
  }
  const auto& obj = value.get<picojson::object>();
  if (obj.find("type") != obj.end() &&
      (!obj["type"].is<std::string>() || obj["type"].get<std::string>() != "tag")) {
    return ResultErr("Tag format's type must be a string \"tag\"");
  }
  return ParseTagFormat(obj);
}

Result<TagFormat> StructuralTagImpl::ParseTagFormat(const picojson::object& obj) {
  // begin is required.
  auto begin_it = obj.find("begin");
  if (begin_it == obj.end() || !begin_it->second.is<std::string>() ||
      begin_it->second.get<std::string>().empty()) {
    return ResultErr("Tag format must have a begin field with a non-empty string");
  }
  // content is required.
  auto content_it = obj.find("content");
  if (content_it == obj.end()) {
    return ResultErr("Tag format must have a content field");
  }
  auto content = ParseFormat(content_it->second);
  if (content.IsErr()) {
    return ResultErr(std::move(content).UnwrapErr());
  }
  // end is required.
  auto detected_end_str_it = obj.find("end");
  if (detected_end_str_it == obj.end() || !detected_end_str_it->second.is<std::string>() ||
      detected_end_str_it->second.get<std::string>().empty()) {
    return ResultErr("Tag format must have an end field with a non-empty string");
  }
  return ResultOk<TagFormat>(
      begin_it->second.get<std::string>(),
      std::make_unique<Format>(std::move(content).Unwrap()),
      detected_end_str_it->second.get<std::string>()
  );
}

Result<TriggeredTagsFormat> StructuralTagImpl::ParseTriggeredTagsFormat(const picojson::object& obj
) {
  // triggers is required.
  auto triggers_it = obj.find("triggers");
  if (triggers_it == obj.end() || !triggers_it->second.is<picojson::array>()) {
    return ResultErr("Triggered tags format must have a triggers field with an array");
  }
  const auto& triggers_array = triggers_it->second.get<picojson::array>();
  std::vector<std::string> triggers;
  triggers.reserve(triggers_array.size());
  for (const auto& trigger : triggers_array) {
    if (!trigger.is<std::string>() || trigger.get<std::string>().empty()) {
      return ResultErr("Triggers must be non-empty strings");
    }
    triggers.push_back(trigger.get<std::string>());
  }
  // tags is required.
  auto tags_it = obj.find("tags");
  if (tags_it == obj.end() || !tags_it->second.is<picojson::array>()) {
    return ResultErr("Triggered tags format must have a tags field with an array");
  }
  const auto& tags_array = tags_it->second.get<picojson::array>();
  std::vector<TagFormat> tags;
  tags.reserve(tags_array.size());
  for (const auto& tag : tags_array) {
    auto tag_format = ParseTagFormat(tag);
    if (tag_format.IsErr()) {
      return ResultErr(std::move(tag_format).UnwrapErr());
    }
    tags.push_back(std::move(tag_format).Unwrap());
  }
  // at_least_one is optional.
  bool at_least_one = false;
  auto at_least_one_it = obj.find("at_least_one");
  if (at_least_one_it != obj.end()) {
    if (!at_least_one_it->second.is<bool>()) {
      return ResultErr("at_least_one must be a boolean");
    }
    at_least_one = at_least_one_it->second.get<bool>();
  }
  // stop_after_first is optional.
  bool stop_after_first = false;
  auto stop_after_first_it = obj.find("stop_after_first");
  if (stop_after_first_it != obj.end()) {
    if (!stop_after_first_it->second.is<bool>()) {
      return ResultErr("stop_after_first must be a boolean");
    }
    stop_after_first = stop_after_first_it->second.get<bool>();
  }
  return ResultOk<TriggeredTagsFormat>(
      std::move(triggers), std::move(tags), at_least_one, stop_after_first
  );
}

Result<TagsWithSeparatorFormat> StructuralTagImpl::ParseTagsWithSeparatorFormat(
    const picojson::object& obj
) {
  // tags is required.
  auto tags_it = obj.find("tags");
  if (tags_it == obj.end() || !tags_it->second.is<picojson::array>()) {
    return ResultErr("Tags with separator format must have a tags field with an array");
  }
  const auto& tags_array = tags_it->second.get<picojson::array>();
  std::vector<TagFormat> tags;
  tags.reserve(tags_array.size());
  for (const auto& tag : tags_array) {
    auto tag_format = ParseTagFormat(tag);
    if (tag_format.IsErr()) {
      return ResultErr(std::move(tag_format).UnwrapErr());
    }
    tags.push_back(std::move(tag_format).Unwrap());
  }
  // separator is required.
  auto separator_it = obj.find("separator");
  if (separator_it == obj.end() || !separator_it->second.is<std::string>() ||
      separator_it->second.get<std::string>().empty()) {
    return ResultErr(
        "Tags with separator format must have a separator field with a non-empty string"
    );
  }
  // at_least_one is optional.
  bool at_least_one = false;
  auto at_least_one_it = obj.find("at_least_one");
  if (at_least_one_it != obj.end()) {
    if (!at_least_one_it->second.is<bool>()) {
      return ResultErr("at_least_one must be a boolean");
    }
    at_least_one = at_least_one_it->second.get<bool>();
  }
  // stop_after_first is optional.
  bool stop_after_first = false;
  auto stop_after_first_it = obj.find("stop_after_first");
  if (stop_after_first_it != obj.end()) {
    if (!stop_after_first_it->second.is<bool>()) {
      return ResultErr("stop_after_first must be a boolean");
    }
    stop_after_first = stop_after_first_it->second.get<bool>();
  }
  return ResultOk<TagsWithSeparatorFormat>(
      std::move(tags), separator_it->second.get<std::string>(), at_least_one, stop_after_first
  );
}

/************** StructuralTag Methods **************/

std::variant<StructuralTag, std::runtime_error> StructuralTag::FromJSON(const std::string& json) {
  return StructuralTagImpl::FromJSON(json).ToVariant();
}

/************** StructuralTag Analyzer **************/

/*!
 * \brief Analyze a StructuralTag and extract useful information for conversion to Grammar.
 */
class StructuralTagAnalyzer {
 public:
  static std::optional<std::runtime_error> Analyze(StructuralTag* structural_tag);

 private:
  /*! \brief A variant that can hold the pointer of any Format types. */
  using FormatPtrVariant = std::variant<
      LiteralFormat*,
      JSONSchemaFormat*,
      WildcardTextFormat*,
      SequenceFormat*,
      OrFormat*,
      TagFormat*,
      TriggeredTagsFormat*,
      TagsWithSeparatorFormat*>;

  // Call this if we have a pointer to a Format.
  std::optional<std::runtime_error> Visit(Format* format);
  // Call this if we have a pointer to a variant of Format.
  std::optional<std::runtime_error> Visit(FormatPtrVariant format);

  // The following is dispatched from VisitFormat. Don't call them directly because they don't
  // handle stack logics.
  std::optional<std::runtime_error> Visit(LiteralFormat* format);
  std::optional<std::runtime_error> Visit(JSONSchemaFormat* format);
  std::optional<std::runtime_error> Visit(WildcardTextFormat* format);
  std::optional<std::runtime_error> Visit(SequenceFormat* format);
  std::optional<std::runtime_error> Visit(OrFormat* format);
  std::optional<std::runtime_error> Visit(TagFormat* format);
  std::optional<std::runtime_error> Visit(TriggeredTagsFormat* format);
  std::optional<std::runtime_error> Visit(TagsWithSeparatorFormat* format);

  std::optional<std::string> DetectEndString();
  bool IsUnlimited(const Format& format);

  int visit_format_recursion_depth_ = 0;
  std::vector<FormatPtrVariant> stack_;
};

std::optional<std::runtime_error> StructuralTagAnalyzer::Analyze(StructuralTag* structural_tag) {
  return StructuralTagAnalyzer().Visit(&structural_tag->format);
}

std::optional<std::string> StructuralTagAnalyzer::DetectEndString() {
  for (int i = static_cast<int>(stack_.size()) - 1; i >= 0; --i) {
    auto& format = stack_[i];

    if (std::holds_alternative<TagFormat*>(format)) {
      auto* tag = std::get<TagFormat*>(format);
      return tag->end;
    }
  }
  return std::nullopt;
}

bool StructuralTagAnalyzer::IsUnlimited(const Format& format) {
  return std::visit(
      [&](auto&& arg) -> bool {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, WildcardTextFormat>) {
          return true;
        } else if constexpr (std::is_same_v<T, TriggeredTagsFormat>) {
          return true;
        } else if constexpr (std::is_same_v<T, TagsWithSeparatorFormat>) {
          return true;
        } else if constexpr (std::is_same_v<T, SequenceFormat>) {
          return arg.is_unlimited_;
        } else if constexpr (std::is_same_v<T, OrFormat>) {
          return arg.is_unlimited_;
        } else {
          return false;
        }
      },
      format
  );
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(Format* format) {
  FormatPtrVariant format_ptr_variant =
      std::visit([&](auto&& arg) -> FormatPtrVariant { return &arg; }, *format);
  return Visit(format_ptr_variant);
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(FormatPtrVariant format) {
  RecursionGuard guard(&visit_format_recursion_depth_);

  // Push format to stack
  stack_.push_back(format);

  // Dispatch to the corresponding visit function
  auto result = std::visit(
      [&](auto&& arg) -> std::optional<std::runtime_error> { return Visit(arg); }, format
  );

  // Pop format from stack
  stack_.pop_back();

  return result;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(LiteralFormat* format) {
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(JSONSchemaFormat* format) {
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(WildcardTextFormat* format) {
  format->detected_end_str_ = DetectEndString();
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(SequenceFormat* format) {
  for (size_t i = 0; i < format->elements.size() - 1; ++i) {
    auto& element = format->elements[i];
    auto err = Visit(&element);
    if (err.has_value()) {
      return err;
    }
    if (IsUnlimited(element)) {
      return std::runtime_error(
          "Only the last element in a sequence can be unlimited, but the " + std::to_string(i) +
          "th element of sequence format is unlimited"
      );
    }
  }

  auto& element = format->elements.back();
  auto err = Visit(&element);
  if (err.has_value()) {
    return err;
  }
  format->is_unlimited_ = IsUnlimited(element);
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(OrFormat* format) {
  bool is_any_unlimited = false;
  bool is_all_unlimited = true;
  for (auto& element : format->elements) {
    auto err = Visit(&element);
    if (err.has_value()) {
      return err;
    }
    auto is_unlimited = IsUnlimited(element);
    is_any_unlimited |= is_unlimited;
    is_all_unlimited &= is_unlimited;
  }

  if (is_any_unlimited && !is_all_unlimited) {
    return std::runtime_error(
        "Now we only support all elements in an or format to be unlimited or all limited, but the "
        "or format has both unlimited and limited elements"
    );
  }

  format->is_unlimited_ = is_any_unlimited;
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(TagFormat* format) {
  auto err = Visit(format->content.get());
  if (err.has_value()) {
    return err;
  }
  auto is_content_unlimited = IsUnlimited(*(format->content));
  if (is_content_unlimited) {
    if (format->end.empty()) {
      return std::runtime_error(
          "When the content is unlimited, the end of the tag format cannot be empty"
      );
    }
    // Clear the end string because it is moved to the detected_end_str_ field.
    format->end.clear();
  }
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(TriggeredTagsFormat* format) {
  for (auto& tag : format->tags) {
    auto err = Visit(&tag);
    if (err.has_value()) {
      return err;
    }
  }
  format->detected_end_str_ = DetectEndString();
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::Visit(TagsWithSeparatorFormat* format) {
  for (auto& tag : format->tags) {
    auto err = Visit(&tag);
    if (err.has_value()) {
      return err;
    }
  }
  format->detected_end_str_ = DetectEndString();
  return std::nullopt;
}

/************** StructuralTag to Grammar Converter **************/

class StructuralTagGrammarConverter {
 public:
  static Result<Grammar> Convert(const StructuralTag& structural_tag);

 private:
  std::optional<std::runtime_error> Visit(const Format& format);
  std::optional<std::runtime_error> Visit(const LiteralFormat& format);
  std::optional<std::runtime_error> Visit(const JSONSchemaFormat& format);
  std::optional<std::runtime_error> Visit(const WildcardTextFormat& format);
  std::optional<std::runtime_error> Visit(const SequenceFormat& format);
  std::optional<std::runtime_error> Visit(const OrFormat& format);
  std::optional<std::runtime_error> Visit(const TagFormat& format);
  std::optional<std::runtime_error> Visit(const TriggeredTagsFormat& format);
  std::optional<std::runtime_error> Visit(const TagsWithSeparatorFormat& format);

  GrammarBuilder grammar_builder_;
};

Result<Grammar> StructuralTagGrammarConverter::Convert(const StructuralTag& structural_tag) {
  auto converter = StructuralTagGrammarConverter();
  auto err = converter.Visit(structural_tag.format);
  if (err.has_value()) {
    return ResultErr(std::move(err).value());
  }
  auto grammar = converter.grammar_builder_.Get();
  return ResultOk(std::move(grammar));
}

/************** StructuralTag to Grammar Public API **************/

Result<Grammar> StructuralTagToGrammar(const std::string& structural_tag_json) {
  auto structural_tag_result = StructuralTagImpl().FromJSON(structural_tag_json);
  if (structural_tag_result.IsErr()) {
    return ResultErr(std::move(structural_tag_result).UnwrapErr());
  }
  auto structural_tag = std::move(structural_tag_result).Unwrap();
  return StructuralTagToGrammar(&structural_tag);
}

Result<Grammar> StructuralTagToGrammar(StructuralTag* structural_tag) {
  auto err = StructuralTagAnalyzer().Analyze(structural_tag);
  if (err.has_value()) {
    return ResultErr(std::move(err).value());
  }
  return StructuralTagGrammarConverter().Convert(*structural_tag);
}

// Grammar StructuralTagToGrammar(
//     const std::vector<StructuralTagItem>& tags, const std::vector<std::string>& triggers
// ) {
//   // Step 1: handle triggers. Triggers should not be mutually inclusive
//   std::vector<std::string> sorted_triggers(triggers.begin(), triggers.end());
//   std::sort(sorted_triggers.begin(), sorted_triggers.end());
//   for (int i = 0; i < static_cast<int>(sorted_triggers.size()) - 1; ++i) {
//     XGRAMMAR_CHECK(
//         sorted_triggers[i + 1].size() < sorted_triggers[i].size() ||
//         std::string_view(sorted_triggers[i + 1]).substr(0, sorted_triggers[i].size()) !=
//             sorted_triggers[i]
//     ) << "Triggers should not be mutually inclusive, but "
//       << sorted_triggers[i] << " is a prefix of " << sorted_triggers[i + 1];
//   }

//   // Step 2: For each tag, find the trigger that is a prefix of the tag.begin
//   // Convert the schema to grammar at the same time
//   std::vector<Grammar> schema_grammars;
//   schema_grammars.reserve(tags.size());
//   for (const auto& tag : tags) {
//     auto schema_grammar = Grammar::FromJSONSchema(tag.schema, true);
//     schema_grammars.push_back(schema_grammar);
//   }

//   std::vector<std::vector<std::pair<StructuralTagItem, Grammar>>> tag_groups(triggers.size());
//   for (int it_tag = 0; it_tag < static_cast<int>(tags.size()); ++it_tag) {
//     const auto& tag = tags[it_tag];
//     bool found = false;
//     for (int it_trigger = 0; it_trigger < static_cast<int>(sorted_triggers.size());
//     ++it_trigger)
//     {
//       const auto& trigger = sorted_triggers[it_trigger];
//       if (trigger.size() <= tag.begin.size() &&
//           std::string_view(tag.begin).substr(0, trigger.size()) == trigger) {
//         tag_groups[it_trigger].push_back(std::make_pair(tag, schema_grammars[it_tag]));
//         found = true;
//         break;
//       }
//     }
//     XGRAMMAR_CHECK(found) << "Tag " << tag.begin << " does not match any trigger";
//   }

//   // Step 3: Combine the tags to form a grammar
//   // root ::= TagDispatch((trigger1, rule1), (trigger2, rule2), ...)
//   // Suppose tag1 and tag2 matches trigger1, then
//   // rule1 ::= (tag1.begin[trigger1.size():] + ToEBNF(tag1.schema) + tag1.end) |
//   //            (tag2.begin[trigger1.size():] + ToEBNF(tag2.schema) + tag2.end) | ...
//   //
//   // Suppose tag3 matches trigger2, then
//   // rule2 ::= (tag3.begin[trigger2.size():] + ToEBNF(tag3.schema) + tag3.end)
//   //
//   // ...
//   return StructuralTagGrammarCreator::Apply(sorted_triggers, tag_groups);
// }

}  // namespace xgrammar
