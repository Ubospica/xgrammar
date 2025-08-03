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
  Result<StructuralTag> FromJSON(const std::string& json);

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
    return Result<StructuralTag>::Err("Failed to parse JSON: " + err);
  }
  return ParseStructuralTag(value);
}

Result<StructuralTag> StructuralTagImpl::ParseStructuralTag(const picojson::value& value) {
  if (!value.is<picojson::object>()) {
    return Result<StructuralTag>::Err("Structural tag must be an object");
  }
  const auto& obj = value.get<picojson::object>();
  // The type field is optional but must be "structural_tag" if present.
  if (obj.find("type") != obj.end()) {
    if (!obj["type"].is<std::string>() || obj["type"].get<std::string>() != "structural_tag") {
      return Result<StructuralTag>::Err("Structural tag's type must be a string \"structural_tag\""
      );
    }
  }
  // The format field is required.
  if (obj.find("format") == obj.end()) {
    return Result<StructuralTag>::Err("Structural tag must have a format field");
  }
  auto format = ParseFormat(obj["format"]);
  if (format.IsErr()) {
    return Result<StructuralTag>::Err(std::move(format).UnwrapErr());
  }
  return Result<StructuralTag>::Ok(StructuralTag(std::move(format).Unwrap()));
}

Result<Format> StructuralTagImpl::ParseFormat(const picojson::value& value) {
  RecursionGuard guard(&parse_format_recursion_depth_);
  if (!value.is<picojson::object>()) {
    return Result<Format>::Err("Format must be an object");
  }
  const auto& obj = value.get<picojson::object>();
  // If type is present, use it to determine the format.
  if (obj.find("type") != obj.end()) {
    if (!obj["type"].is<std::string>()) {
      return Result<Format>::Err("Format's type must be a string");
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
    } else if (type == "tag") {
      return Result<Format>::Convert(ParseTagFormat(obj));
    } else if (type == "triggered_tags") {
      return Result<Format>::Convert(ParseTriggeredTagsFormat(obj));
    } else if (type == "tags_with_separator") {
      return Result<Format>::Convert(ParseTagsWithSeparatorFormat(obj));
    } else {
      return Result<Format>::Err("Invalid format type: " + type);
    }
  }
  // If type is not present, try every format type one by one. Tag is prioritized.
  auto tag_format = ParseTagFormat(obj);
  if (!tag_format.IsErr()) {
    return Result<Format>::Ok(std::move(tag_format).Unwrap());
  }
  auto literal_format = ParseLiteralFormat(obj);
  if (!literal_format.IsErr()) {
    return Result<Format>::Ok(std::move(literal_format).Unwrap());
  }
  auto json_schema_format = ParseJSONSchemaFormat(obj);
  if (!json_schema_format.IsErr()) {
    return Result<Format>::Ok(std::move(json_schema_format).Unwrap());
  }
  auto wildcard_text_format = ParseWildcardTextFormat(obj);
  if (!wildcard_text_format.IsErr()) {
    return Result<Format>::Ok(std::move(wildcard_text_format).Unwrap());
  }
  auto sequence_format = ParseSequenceFormat(obj);
  if (!sequence_format.IsErr()) {
    return Result<Format>::Ok(std::move(sequence_format).Unwrap());
  }
  auto triggered_tags_format = ParseTriggeredTagsFormat(obj);
  if (!triggered_tags_format.IsErr()) {
    return Result<Format>::Ok(std::move(triggered_tags_format).Unwrap());
  }
  auto tags_with_separator_format = ParseTagsWithSeparatorFormat(obj);
  if (!tags_with_separator_format.IsErr()) {
    return Result<Format>::Ok(std::move(tags_with_separator_format).Unwrap());
  }
  return Result<Format>::Err("Invalid format: " + value.serialize(false));
}

Result<LiteralFormat> StructuralTagImpl::ParseLiteralFormat(const picojson::object& obj) {
  // text is required.
  auto text_it = obj.find("text");
  if (text_it == obj.end() || !text_it->second.is<std::string>() ||
      text_it->second.get<std::string>().empty()) {
    return Result<LiteralFormat>::Err(
        "Literal format must have a text field with a non-empty string"
    );
  }
  return Result<LiteralFormat>::Ok(text_it->second.get<std::string>());
}

Result<JSONSchemaFormat> StructuralTagImpl::ParseJSONSchemaFormat(const picojson::object& obj) {
  // json_schema is required.
  auto json_schema_it = obj.find("json_schema");
  if (json_schema_it == obj.end() || !json_schema_it->second.is<picojson::object>()) {
    return Result<JSONSchemaFormat>::Err(
        "JSON schema format must have a json_schema field with a JSON object"
    );
  }
  // here introduces a serialization/deserialization overhead; try to avoid it in the future.
  return Result<JSONSchemaFormat>::Ok(json_schema_it->second.serialize(false));
}

Result<WildcardTextFormat> StructuralTagImpl::ParseWildcardTextFormat(const picojson::object& obj) {
  // obj should not have any fields other than "type"
  if (obj.size() > 1 || (obj.size() == 1 && obj.begin()->first != "type")) {
    return Result<WildcardTextFormat>::Err(
        "Wildcard text format should not have any fields other than type"
    );
  }
  return Result<WildcardTextFormat>::Ok();
}

Result<SequenceFormat> StructuralTagImpl::ParseSequenceFormat(const picojson::object& obj) {
  // elements is required.
  auto elements_it = obj.find("elements");
  if (elements_it == obj.end() || !elements_it->second.is<picojson::array>()) {
    return Result<SequenceFormat>::Err("Sequence format must have an elements field with an array");
  }
  const auto& elements_array = elements_it->second.get<picojson::array>();
  std::vector<Format> elements;
  elements.reserve(elements_array.size());
  for (const auto& element : elements_array) {
    auto format = ParseFormat(element);
    if (format.IsErr()) {
      return Result<SequenceFormat>::Err(std::move(format).UnwrapErr());
    }
    elements.push_back(std::move(format).Unwrap());
  }
  return Result<SequenceFormat>::Ok(std::move(elements));
}

Result<TagFormat> StructuralTagImpl::ParseTagFormat(const picojson::value& value) {
  if (!value.is<picojson::object>()) {
    return Result<TagFormat>::Err("Tag format must be an object");
  }
  const auto& obj = value.get<picojson::object>();
  if (obj.find("type") != obj.end() &&
      (!obj["type"].is<std::string>() || obj["type"].get<std::string>() != "tag")) {
    return Result<TagFormat>::Err("Tag format's type must be a string \"tag\"");
  }
  return ParseTagFormat(obj);
}

Result<TagFormat> StructuralTagImpl::ParseTagFormat(const picojson::object& obj) {
  // begin is required.
  auto begin_it = obj.find("begin");
  if (begin_it == obj.end() || !begin_it->second.is<std::string>() ||
      begin_it->second.get<std::string>().empty()) {
    return Result<TagFormat>::Err("Tag format must have a begin field with a non-empty string");
  }
  // content is required.
  auto content_it = obj.find("content");
  if (content_it == obj.end()) {
    return Result<TagFormat>::Err("Tag format must have a content field");
  }
  auto content = ParseFormat(content_it->second);
  if (content.IsErr()) {
    return Result<TagFormat>::Err(std::move(content).UnwrapErr());
  }
  // end is required.
  auto end_it = obj.find("end");
  if (end_it == obj.end() || !end_it->second.is<std::string>() ||
      end_it->second.get<std::string>().empty()) {
    return Result<TagFormat>::Err("Tag format must have an end field with a non-empty string");
  }
  return Result<TagFormat>::Ok(
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
    return Result<TriggeredTagsFormat>::Err(
        "Triggered tags format must have a triggers field with an array"
    );
  }
  const auto& triggers_array = triggers_it->second.get<picojson::array>();
  std::vector<std::string> triggers;
  triggers.reserve(triggers_array.size());
  for (const auto& trigger : triggers_array) {
    if (!trigger.is<std::string>() || trigger.get<std::string>().empty()) {
      return Result<TriggeredTagsFormat>::Err("Triggers must be non-empty strings");
    }
    triggers.push_back(trigger.get<std::string>());
  }
  // tags is required.
  auto tags_it = obj.find("tags");
  if (tags_it == obj.end() || !tags_it->second.is<picojson::array>()) {
    return Result<TriggeredTagsFormat>::Err(
        "Triggered tags format must have a tags field with an array"
    );
  }
  const auto& tags_array = tags_it->second.get<picojson::array>();
  std::vector<TagFormat> tags;
  tags.reserve(tags_array.size());
  for (const auto& tag : tags_array) {
    auto tag_format = ParseTagFormat(tag);
    if (tag_format.IsErr()) {
      return Result<TriggeredTagsFormat>::Err(std::move(tag_format).UnwrapErr());
    }
    tags.push_back(std::move(tag_format).Unwrap());
  }
  // at_least_one is optional.
  bool at_least_one = false;
  auto at_least_one_it = obj.find("at_least_one");
  if (at_least_one_it != obj.end()) {
    if (!at_least_one_it->second.is<bool>()) {
      return Result<TriggeredTagsFormat>::Err("at_least_one must be a boolean");
    }
    at_least_one = at_least_one_it->second.get<bool>();
  }
  // stop_after_first is optional.
  bool stop_after_first = false;
  auto stop_after_first_it = obj.find("stop_after_first");
  if (stop_after_first_it != obj.end()) {
    if (!stop_after_first_it->second.is<bool>()) {
      return Result<TriggeredTagsFormat>::Err("stop_after_first must be a boolean");
    }
    stop_after_first = stop_after_first_it->second.get<bool>();
  }
  return Result<TriggeredTagsFormat>::Ok(
      std::move(triggers), std::move(tags), at_least_one, stop_after_first
  );
}

Result<TagsWithSeparatorFormat> StructuralTagImpl::ParseTagsWithSeparatorFormat(
    const picojson::object& obj
) {
  // tags is required.
  auto tags_it = obj.find("tags");
  if (tags_it == obj.end() || !tags_it->second.is<picojson::array>()) {
    return Result<TagsWithSeparatorFormat>::Err(
        "Tags with separator format must have a tags field with an array"
    );
  }
  const auto& tags_array = tags_it->second.get<picojson::array>();
  std::vector<TagFormat> tags;
  tags.reserve(tags_array.size());
  for (const auto& tag : tags_array) {
    auto tag_format = ParseTagFormat(tag);
    if (tag_format.IsErr()) {
      return Result<TagsWithSeparatorFormat>::Err(std::move(tag_format).UnwrapErr());
    }
    tags.push_back(std::move(tag_format).Unwrap());
  }
  // separator is required.
  auto separator_it = obj.find("separator");
  if (separator_it == obj.end() || !separator_it->second.is<std::string>() ||
      separator_it->second.get<std::string>().empty()) {
    return Result<TagsWithSeparatorFormat>::Err(
        "Tags with separator format must have a separator field with a non-empty string"
    );
  }
  // at_least_one is optional.
  bool at_least_one = false;
  auto at_least_one_it = obj.find("at_least_one");
  if (at_least_one_it != obj.end()) {
    if (!at_least_one_it->second.is<bool>()) {
      return Result<TagsWithSeparatorFormat>::Err("at_least_one must be a boolean");
    }
    at_least_one = at_least_one_it->second.get<bool>();
  }
  // stop_after_first is optional.
  bool stop_after_first = false;
  auto stop_after_first_it = obj.find("stop_after_first");
  if (stop_after_first_it != obj.end()) {
    if (!stop_after_first_it->second.is<bool>()) {
      return Result<TagsWithSeparatorFormat>::Err("stop_after_first must be a boolean");
    }
    stop_after_first = stop_after_first_it->second.get<bool>();
  }
  return Result<TagsWithSeparatorFormat>::Ok(
      std::move(tags), separator_it->second.get<std::string>(), at_least_one, stop_after_first
  );
}

/************** StructuralTag Methods **************/

std::variant<StructuralTag, std::runtime_error> StructuralTag::FromJSON(const std::string& json) {
  return StructuralTagImpl().FromJSON(json).ToVariant();
}

/************** StructuralTag Analyzer **************/

/*!
 * \brief Analyze a StructuralTag and extract useful information for conversion to Grammar.
 */
class StructuralTagAnalyzer {
 public:
  std::optional<std::runtime_error> AnalyzeStructuralTag(StructuralTag* structural_tag);

 private:
  std::optional<std::runtime_error> VisitFormat(Format* format);
  std::optional<std::runtime_error> VisitLiteralFormat(LiteralFormat* format);
  std::optional<std::runtime_error> VisitJSONSchemaFormat(JSONSchemaFormat* format);
  std::optional<std::runtime_error> VisitWildcardTextFormat(WildcardTextFormat* format);
  std::optional<std::runtime_error> VisitSequenceFormat(SequenceFormat* format);
  std::optional<std::runtime_error> VisitTagFormat(TagFormat* format);
  std::optional<std::runtime_error> VisitTriggeredTagsFormat(TriggeredTagsFormat* format);
  std::optional<std::runtime_error> VisitTagsWithSeparatorFormat(TagsWithSeparatorFormat* format);

  std::optional<std::string> DetectEndString();

  struct Frame {
    Format* format;
    int element_id;  // The id of the element currently visited.
  };

  std::vector<Frame> stack_;
  int visit_format_recursion_depth_ = 0;
};

std::optional<std::string> StructuralTagAnalyzer::DetectEndString() {
  for (int i = static_cast<int>(stack_.size()) - 1; i >= 0; --i) {
    auto& frame = stack_[i];

    if (frame.format->type == "tag") {
      return frame.format->end;
    }
  }
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::AnalyzeStructuralTag(
    StructuralTag* structural_tag
) {
  return VisitFormat(&structural_tag->format);
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitFormat(Format* format) {
  RecursionGuard guard(&visit_format_recursion_depth_);
  stack_.push_back({format, 0});
  auto result = std::visit(
      [&](auto&& arg) -> std::optional<std::runtime_error> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, LiteralFormat>) {
          return VisitLiteralFormat(&arg);
        } else if constexpr (std::is_same_v<T, JSONSchemaFormat>) {
          return VisitJSONSchemaFormat(&arg);
        } else if constexpr (std::is_same_v<T, WildcardTextFormat>) {
          return VisitWildcardTextFormat(&arg);
        } else if constexpr (std::is_same_v<T, SequenceFormat>) {
          return VisitSequenceFormat(&arg);
        } else if constexpr (std::is_same_v<T, TagFormat>) {
          return VisitTagFormat(&arg);
        } else if constexpr (std::is_same_v<T, TriggeredTagsFormat>) {
          return VisitTriggeredTagsFormat(&arg);
        } else if constexpr (std::is_same_v<T, TagsWithSeparatorFormat>) {
          return VisitTagsWithSeparatorFormat(&arg);
        } else {
          XGRAMMAR_LOG(FATAL) << "Unhandled format type: " << typeid(T).name();
        }
      },
      *format
  );
  stack_.pop_back();
  return result;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitLiteralFormat(LiteralFormat* format) {
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitJSONSchemaFormat(
    JSONSchemaFormat* format
) {
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitWildcardTextFormat(
    WildcardTextFormat* format
) {
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitSequenceFormat(SequenceFormat* format
) {
  for (auto& element : format->elements) {
    auto result = VisitFormat(&element);
    if (result.has_value()) {
      return result;
    }
    ++stack_.back().element_id;
  }
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitTagFormat(TagFormat* format) {
  return VisitFormat(format->content.get());
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitTriggeredTagsFormat(
    TriggeredTagsFormat* format
) {
  return std::nullopt;
}

std::optional<std::runtime_error> StructuralTagAnalyzer::VisitTagsWithSeparatorFormat(
    TagsWithSeparatorFormat* format
) {
  return std::nullopt;
}

/************** StructuralTag to Grammar Converter **************/

class StructuralTagGrammarConverter {
 public:
  using StructuralTagInternal = STIIR::StructuralTagInternal;

  Result<Grammar> Convert(const std::string& structural_tag_json);
  Result<Grammar> Convert(const StructuralTag& structural_tag);
  Result<Grammar> Convert(const StructuralTagInternal& structural_tag_internal);
};

Result<Grammar> StructuralTagGrammarConverter::Convert(const std::string& structural_tag_json) {
  auto structural_tag = StructuralTagImpl().FromJSON(structural_tag_json);
  if (structural_tag.IsErr()) {
    throw std::runtime_error(std::move(structural_tag).UnwrapErr());
  }
  return Convert(std::move(structural_tag).Unwrap());
}

Result<Grammar> StructuralTagGrammarConverter::Convert(const StructuralTag& structural_tag) {
  auto structural_tag_internal = STIIR::FromStructuralTag(structural_tag);
  if (structural_tag_internal.IsErr()) {
    return Result<Grammar>::Err(std::move(structural_tag_internal).UnwrapErr());
  }
  return Convert(std::move(structural_tag_internal).Unwrap());
}

Result<Grammar> StructuralTagGrammarConverter::Convert(
    const StructuralTagInternal& structural_tag_internal
) {
  return Result<Grammar>::Err("Not implemented");
}

/************** StructuralTag to Grammar Public API **************/

Result<Grammar> StructuralTagToGrammar(const std::string& structural_tag_json) {
  return StructuralTagGrammarConverter().Convert(structural_tag_json);
}

Result<Grammar> StructuralTagToGrammar(const StructuralTag& structural_tag) {
  return StructuralTagGrammarConverter().Convert(structural_tag);
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
