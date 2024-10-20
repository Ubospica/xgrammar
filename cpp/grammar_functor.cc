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
  // Keep the sequence expr in lookahead assertion
  int32_t VisitLookaheadAssertion(int32_t lookahead_assertion_id) final {
    if (lookahead_assertion_id == -1) {
      return -1;
    }
    auto grammar_expr = grammar_->GetGrammarExpr(lookahead_assertion_id);
    XGRAMMAR_CHECK(grammar_expr.type == GrammarExprType::kSequence);

    std::vector<int32_t> sequence_ids;
    for (int32_t i : grammar_expr) {
      sequence_ids.push_back(VisitExpr(i));
    }
    return builder_.AddSequence(sequence_ids);
  }

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

/*!
 * \brief Unwrap the rules containing nested expressions. After unwrapping, each rule will be in
 * the form: `rule_name ::= ("" | (element1_1 element1_2 ...) | (element2_1 element2_2 ...) | ...)`.
 *
 * I.e. a list of choices, each choice is a sequence of elements. Elements can be a character class
 * or a rule reference. And if the rule can be empty, the first choice will be an empty string.
 *
 * \example The rule `A ::= ((a) (((b)) (c)) "")` will be replaced by `A ::= ((a b c))`. One choice
 * containing a sequence of three elements. The empty string is removed.
 * \example The rule `A ::= (a | (b | (c | "")))` will be replaced by
 * `A ::= ("" | (a) | (b) | (c))`. The first choice is an empty string, and each of the other three
 * choices is a sequence containing a single element.
 * \example The rule `A ::= (a | (b (c | d)))` will be replaced by
 * `A ::= ((a) | (b B)), B ::= ((c) | (d))`. A new rule B is created to represent the nested
 * choices.
 */
class NestedRuleUnwrapper : public BNFGrammarMutator {
 public:
  using BNFGrammarMutator::BNFGrammarMutator;

  BNFGrammar Apply(const BNFGrammar& grammar) final {
    Init(grammar);
    for (int i = 0; i < static_cast<int>(grammar_->NumRules()); ++i) {
      builder_.AddEmptyRule(grammar_->GetRule(i).name);
    }
    for (int i = 0; i < static_cast<int>(grammar_->NumRules()); ++i) {
      auto rule = grammar_->GetRule(i);
      auto grammar_expr = grammar_->GetGrammarExpr(rule.body_expr_id);
      cur_rule_name_ = rule.name;
      auto new_body_expr_id = VisitRuleBody(grammar_expr);
      builder_.UpdateRuleBody(i, new_body_expr_id);
      builder_.AddLookaheadAssertion(i, VisitLookaheadAssertion(rule.lookahead_assertion_id));
    }
    return builder_.Get(grammar_->GetRootRule().name);
  }

 private:
  int32_t VisitLookaheadAssertion(int32_t lookahead_assertion_id) final {
    if (lookahead_assertion_id == -1) {
      return -1;
    }
    auto assertion_expr = grammar_->GetGrammarExpr(lookahead_assertion_id);
    return builder_.AddSequence(VisitSequence_(assertion_expr));
  }

  /*! \brief Visit a GrammarExpr as a rule body. */
  int32_t VisitRuleBody(const GrammarExpr& grammar_expr) {
    switch (grammar_expr.type) {
      case GrammarExprType::kSequence:
        return builder_.AddChoices({builder_.AddSequence(VisitSequence_(grammar_expr))});
      case GrammarExprType::kChoices:
        return builder_.AddChoices(VisitChoices_(grammar_expr));
      case GrammarExprType::kEmptyStr:
        return builder_.AddChoices({builder_.AddEmptyStr()});
      case GrammarExprType::kByteString:
      case GrammarExprType::kCharacterClass:
      case GrammarExprType::kCharacterClassStar:
      case GrammarExprType::kRuleRef:
        return builder_.AddChoices({builder_.AddSequence({builder_.AddGrammarExpr(grammar_expr)})});
      default:
        XGRAMMAR_LOG(FATAL) << "Unexpected sequence type: " << static_cast<int>(grammar_expr.type);
    }
  }

  /*!
   * \brief Visit a GrammarExpr containing choices.
   * \returns A list of new choice GrammarExpr ids.
   */
  std::vector<int32_t> VisitChoices_(const GrammarExpr& grammar_expr) {
    std::vector<int32_t> new_choice_ids;
    bool found_empty = false;
    for (auto i : grammar_expr) {
      auto choice_expr = grammar_->GetGrammarExpr(i);
      switch (choice_expr.type) {
        case GrammarExprType::kSequence:
          VisitSequenceInChoices(choice_expr, &new_choice_ids, &found_empty);
          break;
        case GrammarExprType::kChoices:
          VisitChoicesInChoices(choice_expr, &new_choice_ids, &found_empty);
          break;
        case GrammarExprType::kEmptyStr:
          found_empty = true;
          break;
        case GrammarExprType::kByteString:
        case GrammarExprType::kCharacterClass:
        case GrammarExprType::kCharacterClassStar:
        case GrammarExprType::kRuleRef:
          VisitElementInChoices(choice_expr, &new_choice_ids);
          break;
        default:
          XGRAMMAR_LOG(FATAL) << "Unexpected choice type: " << static_cast<int>(choice_expr.type);
      }
    }
    if (found_empty) {
      new_choice_ids.insert(new_choice_ids.begin(), builder_.AddEmptyStr());
    }
    XGRAMMAR_ICHECK(new_choice_ids.size() >= 1);
    return new_choice_ids;
  }

  /*! \brief Visit a sequence GrammarExpr that is one of a list of choices. */
  void VisitSequenceInChoices(
      const GrammarExpr& grammar_expr, std::vector<int32_t>* new_choice_ids, bool* found_empty
  ) {
    auto sub_sequence_ids = VisitSequence_(grammar_expr);
    if (sub_sequence_ids.size() == 0) {
      *found_empty = true;
    } else {
      new_choice_ids->push_back(builder_.AddSequence(sub_sequence_ids));
    }
  }

  /*! \brief Visit a choice GrammarExpr that is one of a list of choices. */
  void VisitChoicesInChoices(
      const GrammarExpr& grammar_expr, std::vector<int32_t>* new_choice_ids, bool* found_empty
  ) {
    auto sub_choice_ids = VisitChoices_(grammar_expr);
    bool contains_empty =
        builder_.GetGrammarExpr(sub_choice_ids[0]).type == GrammarExprType::kEmptyStr;
    if (contains_empty) {
      *found_empty = true;
      new_choice_ids->insert(
          new_choice_ids->end(), sub_choice_ids.begin() + 1, sub_choice_ids.end()
      );
    } else {
      new_choice_ids->insert(new_choice_ids->end(), sub_choice_ids.begin(), sub_choice_ids.end());
    }
  }

  /*! \brief Visit an atom element GrammarExpr that is one of a list of choices. */
  void VisitElementInChoices(
      const GrammarExpr& grammar_expr, std::vector<int32_t>* new_choice_ids
  ) {
    auto sub_expr_id = builder_.AddGrammarExpr(grammar_expr);
    new_choice_ids->push_back(builder_.AddSequence({sub_expr_id}));
  }

  /*!
   * \brief Visit a GrammarExpr containing a sequence.
   * \returns A list of new sequence GrammarExpr ids.
   */
  std::vector<int32_t> VisitSequence_(const GrammarExpr& grammar_expr) {
    std::vector<int32_t> new_sequence_ids;
    for (auto i : grammar_expr) {
      auto element_expr = grammar_->GetGrammarExpr(i);
      switch (element_expr.type) {
        case GrammarExprType::kSequence:
          VisitSequenceInSequence(element_expr, &new_sequence_ids);
          break;
        case GrammarExprType::kChoices:
          VisitChoiceInSequence(element_expr, &new_sequence_ids);
          break;
        case GrammarExprType::kEmptyStr:
          break;
        case GrammarExprType::kByteString:
        case GrammarExprType::kCharacterClass:
        case GrammarExprType::kCharacterClassStar:
        case GrammarExprType::kRuleRef:
          VisitElementInSequence(element_expr, &new_sequence_ids);
          break;
        default:
          XGRAMMAR_LOG(FATAL) << "Unexpected sequence type: "
                              << static_cast<int>(element_expr.type);
      }
    }
    return new_sequence_ids;
  }

  /*! \brief Visit a sequence GrammarExpr that is one element in another sequence. */
  void VisitSequenceInSequence(
      const GrammarExpr& grammar_expr, std::vector<int32_t>* new_sequence_ids
  ) {
    auto sub_sequence_ids = VisitSequence_(grammar_expr);
    new_sequence_ids->insert(
        new_sequence_ids->end(), sub_sequence_ids.begin(), sub_sequence_ids.end()
    );
  }

  /*! \brief Visit a choice GrammarExpr that is one element in a sequence. */
  void VisitChoiceInSequence(
      const GrammarExpr& grammar_expr, std::vector<int32_t>* new_sequence_ids
  ) {
    auto sub_choice_ids = VisitChoices_(grammar_expr);
    if (sub_choice_ids.size() == 1) {
      auto choice_element_expr = builder_.GetGrammarExpr(sub_choice_ids[0]);
      if (choice_element_expr.type != GrammarExprType::kEmptyStr) {
        new_sequence_ids->insert(
            new_sequence_ids->end(), choice_element_expr.begin(), choice_element_expr.end()
        );
      }
    } else {
      auto new_choice_id = builder_.AddChoices(sub_choice_ids);
      auto new_choice_rule_id = builder_.AddRuleWithHint(cur_rule_name_ + "_choice", new_choice_id);
      new_sequence_ids->push_back(builder_.AddRuleRef(new_choice_rule_id));
    }
  }

  /*! \brief Visit an atom element GrammarExpr that is in a sequence. */
  void VisitElementInSequence(
      const GrammarExpr& grammar_expr, std::vector<int32_t>* new_sequence_ids
  ) {
    new_sequence_ids->push_back(builder_.AddGrammarExpr(grammar_expr));
  }
};

// Return the list of all normalizers in the class. The normalizers are applied one by one.
std::vector<std::unique_ptr<BNFGrammarMutator>> BNFGrammarNormalizer::GetNormalizerList() {
  std::vector<std::unique_ptr<BNFGrammarMutator>> normalizer_mutators;
  normalizer_mutators.emplace_back(std::make_unique<SingleElementExprEliminator>());
  normalizer_mutators.emplace_back(std::make_unique<NestedRuleUnwrapper>());
  return normalizer_mutators;
}

BNFGrammar BNFGrammarNormalizer::Apply(const BNFGrammar& grammar) {
  return SingleElementExprEliminator(grammar).Apply(grammar);
}

}  // namespace xgrammar
