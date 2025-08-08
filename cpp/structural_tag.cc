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
#include "grammar_impl.h"
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
  if (elements.size() == 0) {
    return ResultErr("Sequence format must have at least one element");
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
  if (elements.size() == 0) {
    return ResultErr("Or format must have at least one element");
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
  if (begin_it == obj.end() || !begin_it->second.is<std::string>()) {
    return ResultErr("Tag format's begin field must be a string");
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
  auto end_it = obj.find("end");
  if (end_it == obj.end() || !end_it->second.is<std::string>()) {
    return ResultErr("Tag format's end field must be a string");
  }
  return ResultOk<TagFormat>(
      begin_it->second.get<std::string>(),
      std::make_shared<Format>(std::move(content).Unwrap()),
      end_it->second.get<std::string>()
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
      return ResultErr("Triggered tags format's triggers must be non-empty strings");
    }
    triggers.push_back(trigger.get<std::string>());
  }
  if (triggers.size() == 0) {
    return ResultErr("Triggered tags format's triggers must be non-empty");
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
  if (tags.size() == 0) {
    return ResultErr("Triggered tags format's tags must be non-empty");
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
  if (tags.size() == 0) {
    return ResultErr("Tags with separator format's tags must be non-empty");
  }
  // separator is required.
  auto separator_it = obj.find("separator");
  if (separator_it == obj.end() || !separator_it->second.is<std::string>() ||
      separator_it->second.get<std::string>().empty()) {
    return ResultErr("Tags with separator format's separator field must be a non-empty string");
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

  // The following is dispatched from Visit. Don't call them directly because they don't handle
  // stack logics.
  std::optional<std::runtime_error> VisitSub(LiteralFormat* format);
  std::optional<std::runtime_error> VisitSub(JSONSchemaFormat* format);
  std::optional<std::runtime_error> VisitSub(WildcardTextFormat* format);
  std::optional<std::runtime_error> VisitSub(SequenceFormat* format);
  std::optional<std::runtime_error> VisitSub(OrFormat* format);
  std::optional<std::runtime_error> VisitSub(TagFormat* format);
  std::optional<std::runtime_error> VisitSub(TriggeredTagsFormat* format);
  std::optional<std::runtime_error> VisitSub(TagsWithSeparatorFormat* format);

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
      [&](auto&& arg) -> std::optional<std::runtime_error> { return VisitSub(arg); }, format
  );

  // Pop format from stack
  stack_.pop_back();

  return result;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitSub(LiteralFormat* format) {
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitSub(JSONSchemaFormat* format) {
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitSub(WildcardTextFormat* format) {
  format->detected_end_str_ = DetectEndString();
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitSub(SequenceFormat* format) {
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

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitSub(OrFormat* format) {
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

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitSub(TagFormat* format) {
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

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitSub(TriggeredTagsFormat* format) {
  for (auto& tag : format->tags) {
    auto err = Visit(&tag);
    if (err.has_value()) {
      return err;
    }
  }
  format->detected_end_str_ = DetectEndString();
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitSub(TagsWithSeparatorFormat* format) {
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
  Result<int> Visit(const Format& format);
  Result<int> VisitSub(const LiteralFormat& format);
  Result<int> VisitSub(const JSONSchemaFormat& format);
  Result<int> VisitSub(const WildcardTextFormat& format);
  Result<int> VisitSub(const SequenceFormat& format);
  Result<int> VisitSub(const OrFormat& format);
  Result<int> VisitSub(const TagFormat& format);
  Result<int> VisitSub(const TriggeredTagsFormat& format);
  Result<int> VisitSub(const TagsWithSeparatorFormat& format);
  Grammar AddRootRuleAndGetGrammar(int ref_rule_id);

  bool IsPrefix(const std::string& prefix, const std::string& full_str);

  GrammarBuilder grammar_builder_;
};

bool StructuralTagGrammarConverter::IsPrefix(
    const std::string& prefix, const std::string& full_str
) {
  return prefix.size() <= full_str.size() &&
         std::string_view(full_str).substr(0, prefix.size()) == prefix;
}

Result<Grammar> StructuralTagGrammarConverter::Convert(const StructuralTag& structural_tag) {
  auto converter = StructuralTagGrammarConverter();
  auto result = converter.Visit(structural_tag.format);
  if (result.IsErr()) {
    return ResultErr(std::move(result).UnwrapErr());
  }
  // Add a root rule
  return ResultOk(converter.AddRootRuleAndGetGrammar(std::move(result).Unwrap()));
}

Grammar StructuralTagGrammarConverter::AddRootRuleAndGetGrammar(int ref_rule_id) {
  auto expr = grammar_builder_.AddRuleRef(ref_rule_id);
  auto root_rule_id = grammar_builder_.AddRule("root", expr);
  return grammar_builder_.Get(root_rule_id);
}

Result<int> StructuralTagGrammarConverter::Visit(const Format& format) {
  return std::visit([&](auto&& arg) -> Result<int> { return VisitSub(arg); }, format);
}

Result<int> StructuralTagGrammarConverter::VisitSub(const LiteralFormat& format) {
  auto expr = grammar_builder_.AddByteString(format.text);
  return ResultOk(grammar_builder_.AddRuleWithHint("literal", expr));
}

Result<int> StructuralTagGrammarConverter::VisitSub(const JSONSchemaFormat& format) {
  auto sub_grammar = Grammar::FromJSONSchema(format.json_schema);
  auto added_root_rule_id = SubGrammarAdder().Apply(&grammar_builder_, sub_grammar);
  return ResultOk(added_root_rule_id);
}

Result<int> StructuralTagGrammarConverter::VisitSub(const WildcardTextFormat& format) {
  if (format.detected_end_str_.has_value()) {
    XGRAMMAR_DCHECK(!format.detected_end_str_.value().empty())
        << "The detected end string cannot be empty";
    auto tag_dispatch_expr = grammar_builder_.AddTagDispatch(
        Grammar::Impl::TagDispatch{{}, false, {format.detected_end_str_.value()}, false}
    );
    return ResultOk(grammar_builder_.AddRuleWithHint("wildcard_text", tag_dispatch_expr));
  } else {
    auto wildcard_expr = grammar_builder_.AddCharacterClassStar({{0, 0x10FFFF}}, false);
    auto sequence_expr = grammar_builder_.AddSequence({wildcard_expr});
    auto choices_expr = grammar_builder_.AddChoices({sequence_expr});
    return ResultOk(grammar_builder_.AddRuleWithHint("wildcard_text", choices_expr));
  }
}

Result<int> StructuralTagGrammarConverter::VisitSub(const SequenceFormat& format) {
  std::vector<int> rule_ref_ids;
  rule_ref_ids.reserve(format.elements.size());
  for (const auto& element : format.elements) {
    auto result = Visit(element);
    if (result.IsErr()) {
      return result;
    }
    int sub_rule_id = std::move(result).Unwrap();
    rule_ref_ids.push_back(grammar_builder_.AddRuleRef(sub_rule_id));
  }
  auto expr = grammar_builder_.AddChoices({grammar_builder_.AddSequence(rule_ref_ids)});
  return ResultOk(grammar_builder_.AddRuleWithHint("sequence", expr));
}

Result<int> StructuralTagGrammarConverter::VisitSub(const OrFormat& format) {
  std::vector<int> sequence_ids;
  sequence_ids.reserve(format.elements.size());
  for (const auto& element : format.elements) {
    auto result = Visit(element);
    if (result.IsErr()) {
      return result;
    }
    int sub_rule_id = std::move(result).Unwrap();
    auto rule_ref_expr = grammar_builder_.AddRuleRef(sub_rule_id);
    sequence_ids.push_back(grammar_builder_.AddSequence({rule_ref_expr}));
  }
  auto expr = grammar_builder_.AddChoices(sequence_ids);
  return ResultOk(grammar_builder_.AddRuleWithHint("or", expr));
}

Result<int> StructuralTagGrammarConverter::VisitSub(const TagFormat& format) {
  auto result = Visit(*format.content);
  if (result.IsErr()) {
    return result;
  }
  auto sub_rule_id = std::move(result).Unwrap();
  auto begin_expr = grammar_builder_.AddByteString(format.begin);
  auto end_expr = grammar_builder_.AddByteString(format.end);
  auto rule_ref_expr = grammar_builder_.AddRuleRef(sub_rule_id);
  auto sequence_expr = grammar_builder_.AddSequence({begin_expr, rule_ref_expr, end_expr});
  auto choices_expr = grammar_builder_.AddChoices({sequence_expr});
  return ResultOk(grammar_builder_.AddRuleWithHint("tag", choices_expr));
}

Result<int> StructuralTagGrammarConverter::VisitSub(const TriggeredTagsFormat& format) {
  // Step 1. Visit all tags and add to grammar
  std::vector<std::vector<int>> trigger_to_tag_ids(format.triggers.size());
  std::vector<int> tag_content_rule_ids;
  tag_content_rule_ids.reserve(format.tags.size());

  for (int it_tag = 0; it_tag < static_cast<int>(format.tags.size()); ++it_tag) {
    const auto& tag = format.tags[it_tag];
    // Find matched triggers
    int matched_trigger_id = -1;
    for (int it_trigger = 0; it_trigger < static_cast<int>(format.triggers.size()); ++it_trigger) {
      const auto& trigger = format.triggers[it_trigger];
      if (IsPrefix(trigger, tag.begin)) {
        if (matched_trigger_id != -1) {
          return ResultErr("One tag matches multiple triggers in a triggered tags format");
        }
        matched_trigger_id = it_trigger;
        break;
      }
    }
    if (matched_trigger_id == -1) {
      return ResultErr("One tag does not match any trigger in a triggered tags format");
    }
    trigger_to_tag_ids[matched_trigger_id].push_back(it_tag);

    // Add the tag content to grammar
    auto result = Visit(*tag.content);
    if (result.IsErr()) {
      return result;
    }
    tag_content_rule_ids.push_back(std::move(result).Unwrap());
  }

  // at_least_one is implemented as generating any one of the tags first, then do optional
  // triggered tags generation. That means we don't generate any text before the first tag.

  // Step 2. Special Case: at_least_one && stop_after_first.
  // Then we will generate exactly one tag without text. We just do a selection between all tags.
  if (format.at_least_one && format.stop_after_first) {
    std::vector<int> choice_elements;
    for (int it_tag = 0; it_tag < static_cast<int>(format.tags.size()); ++it_tag) {
      const auto& tag = format.tags[it_tag];
      auto begin_expr_id = grammar_builder_.AddByteString(tag.begin);
      auto end_expr_id = grammar_builder_.AddByteString(tag.end);
      auto rule_ref_expr_id = grammar_builder_.AddRuleRef(tag_content_rule_ids[it_tag]);
      choice_elements.push_back(
          grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id, end_expr_id})
      );
    }
    auto choice_expr_id = grammar_builder_.AddChoices(choice_elements);
    return ResultOk(grammar_builder_.AddRuleWithHint("triggered_tags", choice_expr_id));
  }

  // Step 3. Normal Case. We generate mixture of text and triggered tags.
  // - When at_least_one is true, one tag is generated first, then we do triggered tags generation.
  // - When stop_after_first is true, we set loop_after_dispatch of the tag dispatch to false.
  // - When detected_end_str_ is not empty, we use that as the stop_str of the tag dispatch.
  //   Otherwise, we set stop_eos to true to generate until EOS.

  // Step 3.1 Get tag_rule_pairs.
  std::vector<std::pair<std::string, int32_t>> tag_rule_pairs;
  for (int it_trigger = 0; it_trigger < static_cast<int>(format.triggers.size()); ++it_trigger) {
    const auto& trigger = format.triggers[it_trigger];
    std::vector<int> choice_elements;
    for (const auto& tag_id : trigger_to_tag_ids[it_trigger]) {
      const auto& tag = format.tags[tag_id];
      int begin_expr_id = grammar_builder_.AddByteString(tag.begin.substr(trigger.size()));
      int end_expr_id = grammar_builder_.AddByteString(tag.end);
      int rule_ref_expr_id = grammar_builder_.AddRuleRef(tag_content_rule_ids[tag_id]);
      choice_elements.push_back(
          grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id, end_expr_id})
      );
    }
    auto choice_expr_id = grammar_builder_.AddChoices(choice_elements);
    auto sub_rule_id = grammar_builder_.AddRuleWithHint("triggered_tags_group", choice_expr_id);
    tag_rule_pairs.push_back(std::make_pair(trigger, sub_rule_id));
  }

  // Step 3.2 Add TagDispatch.
  int32_t rule_expr_id;
  bool loop_after_dispatch = !format.stop_after_first;
  if (format.detected_end_str_.has_value()) {
    rule_expr_id = grammar_builder_.AddTagDispatch(Grammar::Impl::TagDispatch{
        tag_rule_pairs, false, {format.detected_end_str_.value()}, loop_after_dispatch
    });
  } else {
    rule_expr_id = grammar_builder_.AddTagDispatch(
        Grammar::Impl::TagDispatch{tag_rule_pairs, true, {}, loop_after_dispatch}
    );
  }

  // Step 3.3 Consider at_least_one
  if (format.at_least_one) {
    // Construct the first rule
    std::vector<int> first_choice_elements;
    for (int it_tag = 0; it_tag < static_cast<int>(format.tags.size()); ++it_tag) {
      const auto& tag = format.tags[it_tag];
      auto begin_expr_id = grammar_builder_.AddByteString(tag.begin);
      auto end_expr_id = grammar_builder_.AddByteString(tag.end);
      auto rule_ref_expr_id = grammar_builder_.AddRuleRef(tag_content_rule_ids[it_tag]);
      first_choice_elements.push_back(
          grammar_builder_.AddSequence({begin_expr_id, rule_ref_expr_id, end_expr_id})
      );
    }
    auto first_choice_expr_id = grammar_builder_.AddChoices(first_choice_elements);
    auto first_rule_id =
        grammar_builder_.AddRuleWithHint("triggered_tags_first", first_choice_expr_id);

    // Construct the full rule
    auto tag_dispatch_rule_id =
        grammar_builder_.AddRuleWithHint("triggered_tags_sub", rule_expr_id);
    auto ref_first_rule_expr_id = grammar_builder_.AddRuleRef(first_rule_id);
    auto ref_tag_dispatch_rule_expr_id = grammar_builder_.AddRuleRef(rule_expr_id);
    auto sequence_expr_id =
        grammar_builder_.AddSequence({ref_first_rule_expr_id, ref_tag_dispatch_rule_expr_id});
    rule_expr_id = grammar_builder_.AddChoices({sequence_expr_id});
  }

  auto rule_id = grammar_builder_.AddRuleWithHint("triggered_tags", rule_expr_id);
  return ResultOk(rule_id);
}

Result<int> StructuralTagGrammarConverter::VisitSub(const TagsWithSeparatorFormat& format) {
  return ResultOk(0);
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
