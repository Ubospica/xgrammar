/*!
 *  Copyright (c) 2026 by Contributors
 * \file xgrammar/regex_fsm_cache.h
 * \brief Cache type shared by schema conversion and grammar FSM construction.
 */
#ifndef XGRAMMAR_REGEX_FSM_CACHE_H_
#define XGRAMMAR_REGEX_FSM_CACHE_H_

#include <string>
#include <string_view>
#include <unordered_map>

#include "fsm.h"

namespace xgrammar {

using RegexFSMCache = std::unordered_map<std::string, FSMWithStartEnd>;

inline std::string MakeRegexFSMCacheKey(const std::string& regex, bool json_string) {
  std::string result;
  result.reserve(regex.size() + 1);
  result.push_back(static_cast<char>(json_string));
  result.append(regex);
  return result;
}

// JSON Schema compilation sometimes constructs an automaton that cannot be represented compactly
// by the public regex syntax (for example, the intersection of pattern and decoded-length
// constraints). Such automata are passed through the short-lived compilation cache under an
// unmistakably internal regex spelling. A compiled grammar is self-contained because it serializes
// the resulting per-rule FSM; trying to compile the internal spelling without its cache must fail
// instead of silently changing the language.
inline constexpr char kInternalRegexFSMCachePrefixBytes[] = "\0xgrammar-cached-fsm:";
inline constexpr std::string_view kInternalRegexFSMCachePrefix{
    kInternalRegexFSMCachePrefixBytes, sizeof(kInternalRegexFSMCachePrefixBytes) - 1
};

inline bool IsInternalRegexFSMCachePattern(const std::string& regex) {
  return regex.size() >= kInternalRegexFSMCachePrefix.size() &&
         regex.compare(0, kInternalRegexFSMCachePrefix.size(), kInternalRegexFSMCachePrefix) == 0;
}

}  // namespace xgrammar

#endif  // XGRAMMAR_REGEX_FSM_CACHE_H_
