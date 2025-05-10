/*!
 *  Copyright (c) 2025 by Contributors
 * \file xgrammar/parser_structural_tag.cc
 */

#include "parser_structural_tag.h"

#include <algorithm>
#include <iomanip>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "picojson.h"
#include "support/logging.h"

namespace xgrammar {

/**
 * @brief Parse structural tags from input string
 *
 * @param input The LLM output string to parse
 * @param triggers List of trigger strings that indicate the start of a structural tag
 * @return std::pair<std::string, std::vector<std::tuple<std::string, std::string, std::string>>>
 *         First element: Text outside structural tags, joined together
 *         Second element: List of (start_tag, content, end_tag) tuples for valid structural tags
 */
std::pair<std::string, std::vector<std::tuple<std::string, std::string, std::string>>>
parse_structural_tag(const std::string& input, const std::vector<std::string>& triggers) {
  std::vector<std::tuple<std::string, std::string, std::string>> parsed_tags;
  std::string non_tag_content;

  size_t input_pos = 0;
  const size_t input_length = input.length();
  std::string_view input_view(input);

  while (input_pos < input_length) {
    bool trigger_matched = false;
    std::string matched_trigger;

    for (const auto& trigger : triggers) {
      if (input_pos + trigger.length() <= input_length &&
          input_view.substr(input_pos, trigger.length()) == trigger) {
        trigger_matched = true;
        matched_trigger = trigger;
        break;
      }
    }

    if (!trigger_matched) {
      non_tag_content.push_back(input[input_pos]);
      input_pos++;
      continue;
    }

    size_t tag_start_pos = input_pos;
    input_pos += matched_trigger.length();

    bool is_complete_tag = matched_trigger.back() == '>';
    size_t start_tag_end;

    if (is_complete_tag) {
      start_tag_end = tag_start_pos + matched_trigger.length() - 1;
    } else {
      start_tag_end = input.find('>', input_pos);
      if (start_tag_end == std::string::npos) {
        // Malformed tag: no closing '>' for start tag
        input_pos = tag_start_pos + matched_trigger.length();
        continue;
      }
    }

    std::string start_tag = input.substr(tag_start_pos, start_tag_end - tag_start_pos + 1);

    // Determine the end tag based on the trigger
    std::string end_tag_prefix;
    if (matched_trigger.substr(0, 1) == "<") {
      if (is_complete_tag) {
        // For tags like <ipython>, extract the tag name without the <>
        end_tag_prefix = "</" + matched_trigger.substr(1, matched_trigger.length() - 2);
      } else {
        size_t tag_name_start = matched_trigger.find('=');
        if (tag_name_start != std::string::npos) {
          end_tag_prefix = "</" + matched_trigger.substr(1, tag_name_start - 1);
        } else {
          // tags without =
          end_tag_prefix = "</" + matched_trigger.substr(1);
        }
      }
    } else {
      end_tag_prefix = "</" + matched_trigger.substr(1);
    }

    size_t content_start = start_tag_end + 1;
    size_t end_tag_start = input.find(end_tag_prefix, content_start);

    if (end_tag_start == std::string::npos) {
      input_pos = start_tag_end + 1;
      continue;
    }

    size_t end_tag_end = input.find('>', end_tag_start);
    if (end_tag_end == std::string::npos) {
      input_pos = start_tag_end + 1;
      continue;
    }

    std::string content = input.substr(content_start, end_tag_start - content_start);
    std::string end_tag = input.substr(end_tag_start, end_tag_end - end_tag_start + 1);

    bool is_valid_json = false;

    // Check if content is potentially JSON by looking for opening/closing brackets
    if ((content.find('{') != std::string::npos && content.find('}') != std::string::npos) ||
        (content.find('[') != std::string::npos && content.find(']') != std::string::npos)) {
      int bracket_count = 0;
      int square_bracket_count = 0;
      bool potentially_valid = true;

      for (char c : content) {
        if (c == '{')
          bracket_count++;
        else if (c == '}') {
          bracket_count--;
          if (bracket_count < 0) {
            potentially_valid = false;
            break;
          }
        } else if (c == '[')
          square_bracket_count++;
        else if (c == ']') {
          square_bracket_count--;
          if (square_bracket_count < 0) {
            potentially_valid = false;
            break;
          }
        }
      }

      if (potentially_valid && bracket_count == 0 && square_bracket_count == 0) {
        picojson::value v;
        std::string err = picojson::parse(v, content);
        is_valid_json = err.empty();
      }
    }

    if (is_valid_json || matched_trigger.find("<function=") != 0) {
      // For function tags, we require valid JSON. For other tags, we might be more lenient
      parsed_tags.emplace_back(start_tag, content, end_tag);
    }

    input_pos = end_tag_end + 1;
  }

  return std::make_pair(non_tag_content, parsed_tags);
}

}  // namespace xgrammar
