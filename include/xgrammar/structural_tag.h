/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/structural_tag.h
 * \brief The header for the definition of the structural tag.
 */
#ifndef XGRAMMAR_STRUCTURAL_TAG_H_
#define XGRAMMAR_STRUCTURAL_TAG_H_

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace xgrammar {

/******************** Discriminated Union ********************/

struct LiteralFormat;
struct JSONSchemaFormat;
struct WildcardTextFormat;
struct SequenceFormat;
struct TagFormat;
struct TriggeredTagsFormat;
struct TagsWithSeparatorFormat;

using Format = std::variant<
    LiteralFormat,
    JSONSchemaFormat,
    WildcardTextFormat,
    SequenceFormat,
    TagFormat,
    TriggeredTagsFormat,
    TagsWithSeparatorFormat>;

/******************** Basic Formats ********************/

struct LiteralFormat {
  static constexpr const char* type = "literal";
  std::string text;
  LiteralFormat(std::string text) : text(std::move(text)) {}

  // For StructuralTagAnalyzer
 private:
  bool deprived_ = false;
  friend class StructuralTagAnalyzer;
};

struct JSONSchemaFormat {
  static constexpr const char* type = "json_schema";
  std::string json_schema;
  JSONSchemaFormat(std::string json_schema) : json_schema(std::move(json_schema)) {}
};

struct WildcardTextFormat {
  static constexpr const char* type = "wildcard_text";
  WildcardTextFormat() {}

  // For StructuralTagAnalyzer
 private:
  bool deprived_ = false;
  friend class StructuralTagAnalyzer;
};

/******************** Combinatorial Formats ********************/

struct SequenceFormat {
  static constexpr const char* type = "sequence";
  std::vector<Format> elements;
  SequenceFormat(std::vector<Format> elements) : elements(std::move(elements)) {}
};

struct TagFormat {
  static constexpr const char* type = "tag";
  std::string begin;
  std::shared_ptr<Format> content;
  std::string end;

  TagFormat(std::string begin, std::shared_ptr<Format> content, std::string end)
      : begin(std::move(begin)), content(std::move(content)), end(std::move(end)) {}

  // For StructuralTagAnalyzer
 private:
  bool begin_deprived_ = false;
  bool end_deprived_ = false;
  friend class StructuralTagAnalyzer;
};

struct TriggeredTagsFormat {
  static constexpr const char* type = "triggered_tags";
  std::vector<std::string> triggers;
  std::vector<TagFormat> tags;
  bool at_least_one = false;
  bool stop_after_first = false;

  TriggeredTagsFormat(
      std::vector<std::string> triggers,
      std::vector<TagFormat> tags,
      bool at_least_one,
      bool stop_after_first
  )
      : triggers(std::move(triggers)),
        tags(std::move(tags)),
        at_least_one(at_least_one),
        stop_after_first(stop_after_first) {}

  // For StructuralTagAnalyzer
 private:
  std::optional<std::string> detected_end_string_ = std::nullopt;
  friend class StructuralTagAnalyzer;
};

struct TagsWithSeparatorFormat {
  static constexpr const char* type = "tags_with_separator";
  std::vector<TagFormat> tags;
  std::string separator;
  bool at_least_one = false;
  bool stop_after_first = false;

  TagsWithSeparatorFormat(
      std::vector<TagFormat> tags, std::string separator, bool at_least_one, bool stop_after_first
  )
      : tags(std::move(tags)),
        separator(std::move(separator)),
        at_least_one(at_least_one),
        stop_after_first(stop_after_first) {}

  // For StructuralTagAnalyzer
 private:
  std::optional<std::string> detected_end_string_ = std::nullopt;
  friend class StructuralTagAnalyzer;
};

/******************** Top Level ********************/

struct StructuralTag {
  static constexpr const char* type = "structural_tag";
  Format format;

  StructuralTag(Format format) : format(std::move(format)) {}

  /*!
   * \brief Parse a JSON string into a StructuralTag.
   * \param json The JSON string to parse.
   * \return A StructuralTag if the JSON is valid, otherwise an error message in std::string.
   */
  static std::variant<StructuralTag, std::runtime_error> FromJSON(const std::string& json);
};

}  // namespace xgrammar

#endif  // XGRAMMAR_STRUCTURAL_TAG_H_
