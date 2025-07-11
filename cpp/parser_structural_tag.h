/*!
 *  Copyright (c) 2025 by Contributors
 * \file xgrammar/parser_structural_tag.h
 */

#ifndef XGRAMMAR_PARSER_STRUCTURAL_TAG_H_
#define XGRAMMAR_PARSER_STRUCTURAL_TAG_H_

#include <string>
#include <tuple>
#include <vector>

namespace xgrammar {

/**
 * @brief Parse structural tags from input string
 *
 * This parser identifies structural tags in LLM output based on trigger strings.
 * It extracts the start tag, content, and end tag for each valid structural element.
 * For function tags with JSON content, it validates the JSON syntax using picojson.
 *
 * @param input The LLM output string to parse
 * @param triggers List of trigger strings that indicate the start of a structural tag
 * @return std::pair<std::string, std::vector<std::tuple<std::string, std::string, std::string>>>
 *         First element: Text outside structural tags, joined together
 *         Second element: List of (start_tag, content, end_tag) tuples for valid structural tags
 */
std::pair<std::string, std::vector<std::tuple<std::string, std::string, std::string>>>
parse_structural_tag(const std::string& input, const std::vector<std::string>& triggers);

}  // namespace xgrammar

#endif  // XGRAMMAR_PARSER_STRUCTURAL_TAG_H_
