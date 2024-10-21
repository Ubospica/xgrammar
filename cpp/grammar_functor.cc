/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/grammar_functor.cc
 */

#include "grammar_functor.h"

#include <xgrammar/xgrammar.h>

#include "grammar_data_structure.h"
#include "support/encoding.h"

namespace xgrammar {

/*!
 * \brief Eliminates single-element sequence or choice or character class in the grammar.
 * \example `A ::= choices("a")` --> `A ::= "a"` (the body is a string)
 * \example `A ::= sequence("a")` --> `A ::= "a"` (the body is a string)
 * \example `A ::= [a-a]` --> `A ::= "a"` (the body is a string)
 */
class SingleElementExprEliminator : public BNFGrammarMutator {
 public:
  using BNFGrammarMutator::Apply;
  using BNFGrammarMutator::BNFGrammarMutator;

 private:
  int32_t VisitSequence(const GrammarExpr& grammar_expr) final {
    std::vector<int32_t> sequence_ids;
    for (int32_t i : grammar_expr) {
      sequence_ids.push_back(VisitExpr(i));
    }
    if (sequence_ids.size() == 1) {
      return sequence_ids[0];
    }
    return builder_.AddSequence(sequence_ids);
  }

  int32_t VisitChoices(const GrammarExpr& grammar_expr) final {
    std::vector<int32_t> choice_ids;
    for (int32_t i : grammar_expr) {
      choice_ids.push_back(VisitExpr(i));
    }
    if (choice_ids.size() == 1) {
      return choice_ids[0];
    }
    return builder_.AddChoices(choice_ids);
  }

  int32_t VisitCharacterClass(const GrammarExpr& grammar_expr) final {
    if (grammar_expr.data_len == 3 && grammar_expr[0] == 0 && grammar_expr[1] == grammar_expr[2]) {
      std::string str = PrintAsUTF8(grammar_expr[1]);
      std::vector<int32_t> bytes;
      bytes.reserve(str.size());
      for (char c : str) {
        bytes.push_back(static_cast<int32_t>(c));
      }
      return builder_.AddByteString(bytes);
    }
    return builder_.AddGrammarExpr(grammar_expr);
  }
};

BNFGrammar BNFGrammarNormalizer::Apply(const BNFGrammar& grammar) {
  return SingleElementExprEliminator().Apply(grammar);
}

}  // namespace xgrammar
