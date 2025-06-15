/*!
 *  Copyright (c) 2025 by Contributors
 * \file xgrammar/fsm_builder.h
 */
#ifndef XGRAMMAR_FSM_BUILDER_H_
#define XGRAMMAR_FSM_BUILDER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "fsm.h"
#include "grammar_data_structure.h"
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
};

/*!
 * \brief A builder that converts a list of patterns to a trie-based FSM.
 */
class TrieFSMBuilder {
 public:
  /*!
   * \brief Build a trie-based FSM from a list of patterns.
   * \param patterns The patterns to be built.
   * \param end_states The end states of the FSM. This is the terminal state of each pattern and
   * the order follows the order of patterns.
   * \param allow_overlap Whether to allow overlap between patterns (one being a prefix of the
   * other). It does not allow empty patterns either. If false and there is overlap, will return
   * std::nullopt.
   * \return If success, the FSM with start and end states. Otherwise, std::nullopt.
   */
  static std::optional<FSMWithStartEnd> Build(
      const std::vector<std::string>& patterns,
      std::vector<int32_t>* end_states = nullptr,
      bool allow_overlap = true,
      bool add_back_edges = true
  );
};

class TagDispatchFSMBuilder {
 public:
  /*!
   * \brief Build a FSM from a tag dispatch rule.
   * \param rule_expr The rule expr.
   * \param grammar The grammar that contains the rule expr.
   * \return The FSM with start and end states.
   */
  static std::optional<FSMWithStartEnd> Build(
      const Grammar::Impl::RuleExpr& rule_expr, const Grammar& grammar
  );

  /*!
   * \brief Build a FSM from a tag dispatch rule. Mainly for test purposes.
   * \param tag_dispatch_rules The tag dispatch rules.
   * \param stop_strings The stop strings.
   * \param loop_after_dispatch Whether to loop after dispatch.
   * \param accept_eos Whether to accept EOS.
   * \return The FSM with start and end states.
   */
  static std::optional<FSMWithStartEnd> Build(
      const std::vector<std::pair<std::string, int>>& tag_dispatch_rules,
      const std::vector<std::string>& stop_strings,
      bool loop_after_dispatch,
      bool accept_eos
  );
};

}  // namespace xgrammar

#endif  // XGRAMMAR_FSM_BUILDER_H_
