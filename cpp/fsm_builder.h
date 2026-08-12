/*!
 *  Copyright (c) 2025 by Contributors
 * \file xgrammar/fsm_builder.h
 */
#ifndef XGRAMMAR_FSM_BUILDER_H_
#define XGRAMMAR_FSM_BUILDER_H_

#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

#include "fsm.h"
#include "support/utils.h"

namespace xgrammar {

/*!
 * \brief A builder that converts a regex string to a FSM.
 */
class RegexFSMBuilder {
 public:
  /*!
   * \brief Converts a regex string to a FSM.
   * \param regex The regex string.
   * \return The FSM with start and end states.
   */
  static Result<FSMWithStartEnd> Build(const std::string& regex);

  /*!
   * \brief Converts a regex string to a FSM, then removes the forbidden characters from every
   * character transition. The result accepts the intersection of the regex language and the
   * set of strings that contain no forbidden character. The result language may be empty.
   * \param regex The regex string.
   * \param forbidden_chars The forbidden characters.
   * \return The FSM with start and end states.
   */
  static Result<FSMWithStartEnd> BuildWithForbiddenChars(
      const std::string& regex, const std::bitset<256>& forbidden_chars
  );

  /*!
   * \brief Converts a regex over the decoded contents of a JSON string to an FSM over its encoded
   * body. ASCII characters accept their raw spelling where JSON permits it, all applicable short
   * escapes, and \u00XX; non-ASCII UTF-8 bytes retain their raw spelling.
   * \param regex The regex string.
   * \return The FSM with start and end states.
   */
  static Result<FSMWithStartEnd> BuildForJSONString(const std::string& regex);

  /*! \brief Build a JSON-string FSM after first determinizing the decoded regex. JSON escape
   * spellings are then added through a shared decoder so the resulting byte FSM stays compact. */
  static Result<FSMWithStartEnd> BuildForJSONStringWithDecodedDFA(
      const std::string& regex, int max_num_states
  );
};

/*!
 * \brief A builder that converts a list of patterns to a trie-based FSM.
 */
class TrieFSMBuilder {
 public:
  /*!
   * \brief Build a trie-based FSM from a list of patterns.
   * \param patterns The patterns to be built.
   * \param excluded_patterns The patterns to be excluded.
   * \param end_states The end states of the FSM. This is the terminal state of each pattern and
   * the order follows the order of patterns.
   * \param allow_overlap Whether to allow overlap between patterns (one being a prefix of the
   * other). It does not allow empty patterns either. If false and there is overlap, will return
   * std::nullopt.
   * \param add_back_edges Whether to add back edges to the FSM. This complements the trie to an
   * Aho-Corasick automaton.
   * \return If success, the FSM with start and end states. Otherwise, std::nullopt.
   */
  static std::optional<FSMWithStartEnd> Build(
      const std::vector<std::string>& patterns,
      const std::vector<std::string>& excluded_patterns,
      std::vector<int32_t>* end_states = nullptr,
      bool allow_overlap = true,
      bool add_back_edges = false
  );
};

}  // namespace xgrammar

#endif  // XGRAMMAR_FSM_BUILDER_H_
