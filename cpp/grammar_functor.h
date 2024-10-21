/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/grammar_functor.h
 * \brief The header for the simplification of the BNF AST.
 */

#ifndef XGRAMMAR_GRAMMAR_FUNCTOR_H_
#define XGRAMMAR_GRAMMAR_FUNCTOR_H_

#include <xgrammar/xgrammar.h>

#include <queue>
#include <string>

#include "grammar_builder.h"
#include "grammar_data_structure.h"
#include "grammar_serializer.h"
#include "support/utils.h"

namespace xgrammar {

/*!
 * \brief Visitor of BNFGrammar.
 * \tparam ReturnType The return type of the Apply() function. Denotes the collected information.
 */
template <typename ResultType>
class BNFGrammarVisitor {
 public:
  /*!
   * \brief Constructor.
   * \param grammar The grammar to visit or mutate.
   */
  explicit BNFGrammarVisitor() = default;

  /*!
   * \brief Apply the transformation to the grammar, or visit the grammar.
   * \return The transformed grammar, or the visiting result, or void.
   */
  virtual ResultType Apply(const BNFGrammar& grammar) {
    Init(grammar);
    for (int i = 0; i < static_cast<int>(grammar_->NumRules()); ++i) {
      auto rule = grammar_->GetRule(i);
      cur_rule_name_ = rule.name;
      VisitExpr(rule.body_expr_id);
      VisitLookaheadAssertion(rule.lookahead_assertion_id);
    }
    return ResultType();
  }

  /*! \brief Virtual destructor. */
  virtual ~BNFGrammarVisitor() = default;

 protected:
  using Rule = BNFGrammar::Impl::Rule;
  using GrammarExpr = BNFGrammar::Impl::GrammarExpr;
  using GrammarExprType = BNFGrammar::Impl::GrammarExprType;

  /*! \brief Initialize the functor. Should be called at the beginning of Apply(). */
  virtual void Init(const BNFGrammar& grammar) { grammar_ = grammar; }

  /*! \brief Visit a lookahead assertion expr referred by id. */
  virtual void VisitLookaheadAssertion(int32_t lookahead_assertion_id) {
    if (lookahead_assertion_id == -1) {
      return;
    }
    return VisitExpr(lookahead_assertion_id);
  }

  /*! \brief Visit a GrammarExpr by id. */
  virtual void VisitExpr(int32_t expr_id_in_old_grammar) {
    return VisitExpr(grammar_->GetGrammarExpr(expr_id_in_old_grammar));
  }

  /*! \brief Visit a GrammarExpr. Dispatch to the corresponding Visit function. */
  virtual void VisitExpr(const GrammarExpr& grammar_expr) {
    switch (grammar_expr.type) {
      case GrammarExprType::kSequence:
        return VisitSequence(grammar_expr);
      case GrammarExprType::kChoices:
        return VisitChoices(grammar_expr);
      case GrammarExprType::kEmptyStr:
        return VisitEmptyStr(grammar_expr);
      case GrammarExprType::kByteString:
        return VisitByteString(grammar_expr);
      case GrammarExprType::kCharacterClass:
        return VisitCharacterClass(grammar_expr);
      case GrammarExprType::kStarQuantifier:
      case GrammarExprType::kPlusQuantifier:
      case GrammarExprType::kQuestionQuantifier:
        return VisitQuantifier(grammar_expr);
      case GrammarExprType::kQuantifierRange:
        return VisitQuantifierRange(grammar_expr);
      case GrammarExprType::kRuleRef:
        return VisitRuleRef(grammar_expr);
      default:
        XGRAMMAR_LOG(FATAL) << "Unexpected sequence type: " << static_cast<int>(grammar_expr.type);
        XGRAMMAR_UNREACHABLE_MARKER();
    }
  }

  /*! \brief Visit a choices GrammarExpr. */
  virtual void VisitChoices(const GrammarExpr& grammar_expr) {
    for (auto i : grammar_expr) {
      VisitExpr(i);
    }
  }

  /*! \brief Visit a sequence GrammarExpr. */
  virtual void VisitSequence(const GrammarExpr& grammar_expr) {
    for (auto i : grammar_expr) {
      VisitExpr(i);
    }
  }

  /*! \brief Visit an element GrammarExpr, including empty string, character class, and rule ref.
   */
  virtual void VisitElement(const GrammarExpr& grammar_expr) { return; }

  /*! \brief Visit an empty string GrammarExpr. */
  virtual void VisitEmptyStr(const GrammarExpr& grammar_expr) { VisitElement(grammar_expr); }

  /*! \brief Visit a character class GrammarExpr. */
  virtual void VisitByteString(const GrammarExpr& grammar_expr) { VisitElement(grammar_expr); }

  /*! \brief Visit a character class GrammarExpr. */
  virtual void VisitCharacterClass(const GrammarExpr& grammar_expr) { VisitElement(grammar_expr); }

  /*! \brief Visit a star quantifier GrammarExpr. */
  virtual void VisitQuantifier(const GrammarExpr& grammar_expr) { VisitExpr(grammar_expr[0]); }

  virtual void VisitQuantifierRange(const GrammarExpr& grammar_expr) { VisitExpr(grammar_expr[0]); }

  /*! \brief Visit a rule reference GrammarExpr. */
  virtual void VisitRuleRef(const GrammarExpr& grammar_expr) { return VisitElement(grammar_expr); }

  /*! \brief The grammar to visit or mutate. */
  BNFGrammar grammar_;
  /*! \brief The name of the current rule being visited. */
  std::string cur_rule_name_;
};

/*!
 * \brief Mutator of BNFGrammar. The Apply() function returns the updated grammar.
 * \tparam T The type of the return value of visitor functions. Typical values:
 * - int32_t: the id of the new grammar_expr
 * - void: no return value
 * \tparam ReturnType The type of the return value of the transform function Apply(). Typical
 values
 * are void (for visitor) and BNFGrammar (for mutator).
 */
class BNFGrammarMutator {
 public:
  /*!
   * \brief Constructor.
   * \param grammar The grammar to visit or mutate.
   */
  explicit BNFGrammarMutator() = default;

  /*!
   * \brief Apply the transformation to the grammar, or visit the grammar.
   * \return The transformed grammar, or the visiting result, or void.
   */
  virtual BNFGrammar Apply(const BNFGrammar& grammar) {
    Init(grammar);
    // First add empty rules to ensure the new rule ids the same as the old ones, then update
    // the rule bodies
    for (int i = 0; i < static_cast<int>(grammar_->NumRules()); ++i) {
      builder_.AddEmptyRule(grammar_->GetRule(i).name);
    }
    for (int i = 0; i < static_cast<int>(grammar_->NumRules()); ++i) {
      auto rule = grammar_->GetRule(i);
      cur_rule_name_ = rule.name;
      auto new_body_expr_id = VisitExpr(rule.body_expr_id);
      builder_.UpdateRuleBody(i, new_body_expr_id);
      // Handle lookahead assertion
      builder_.AddLookaheadAssertion(i, VisitLookaheadAssertion(rule.lookahead_assertion_id));
    }
    return builder_.Get(grammar_->GetRootRule().name);
  }

  /*! \brief Virtual destructor. */
  virtual ~BNFGrammarMutator() = default;

 protected:
  using Rule = BNFGrammar::Impl::Rule;
  using GrammarExpr = BNFGrammar::Impl::GrammarExpr;
  using GrammarExprType = BNFGrammar::Impl::GrammarExprType;

  /*! \brief Initialize the functor. Should be called at the beginning of Apply(). */
  virtual void Init(const BNFGrammar& grammar) {
    grammar_ = grammar;
    builder_ = BNFGrammarBuilder();
    cur_rule_name_.clear();
  }

  /*! \brief Visit a lookahead assertion expr referred by id. */
  virtual int32_t VisitLookaheadAssertion(int32_t lookahead_assertion_id) {
    if (lookahead_assertion_id == -1) {
      return -1;
    }
    return VisitExpr(lookahead_assertion_id);
  }

  /*! \brief Visit a GrammarExpr by id. */
  virtual int32_t VisitExpr(int32_t expr_id_in_old_grammar) {
    return VisitExpr(grammar_->GetGrammarExpr(expr_id_in_old_grammar));
  }

  /*! \brief Visit a GrammarExpr. Dispatch to the corresponding Visit function. */
  virtual int32_t VisitExpr(const GrammarExpr& grammar_expr) {
    switch (grammar_expr.type) {
      case GrammarExprType::kSequence:
        return VisitSequence(grammar_expr);
      case GrammarExprType::kChoices:
        return VisitChoices(grammar_expr);
      case GrammarExprType::kEmptyStr:
        return VisitEmptyStr(grammar_expr);
      case GrammarExprType::kByteString:
        return VisitByteString(grammar_expr);
      case GrammarExprType::kCharacterClass:
        return VisitCharacterClass(grammar_expr);
      case GrammarExprType::kStarQuantifier:
      case GrammarExprType::kPlusQuantifier:
      case GrammarExprType::kQuestionQuantifier:
        return VisitQuantifier(grammar_expr);
      case GrammarExprType::kQuantifierRange:
        return VisitQuantifierRange(grammar_expr);
      case GrammarExprType::kRuleRef:
        return VisitRuleRef(grammar_expr);
      default:
        XGRAMMAR_LOG(FATAL) << "Unexpected grammar expr type: "
                            << static_cast<int>(grammar_expr.type);
        XGRAMMAR_UNREACHABLE_MARKER();
    }
  }

  /*! \brief Visit a choices GrammarExpr. */
  virtual int32_t VisitChoices(const GrammarExpr& grammar_expr) {
    std::vector<int32_t> choice_ids;
    for (int32_t i : grammar_expr) {
      choice_ids.push_back(VisitExpr(i));
    }
    return builder_.AddChoices(choice_ids);
  }

  /*! \brief Visit a sequence GrammarExpr. */
  virtual int32_t VisitSequence(const GrammarExpr& grammar_expr) {
    std::vector<int32_t> sequence_ids;
    for (int32_t i : grammar_expr) {
      sequence_ids.push_back(VisitExpr(i));
    }
    return builder_.AddSequence(sequence_ids);
  }

  /*! \brief Visit an element GrammarExpr, including empty string, character class, and rule ref.
   */
  virtual int32_t VisitElement(const GrammarExpr& grammar_expr) {
    return builder_.AddGrammarExpr(grammar_expr);
  }

  /*! \brief Visit an empty string GrammarExpr. */
  virtual int32_t VisitEmptyStr(const GrammarExpr& grammar_expr) {
    return VisitElement(grammar_expr);
  }

  /*! \brief Visit a character class GrammarExpr. */
  virtual int32_t VisitByteString(const GrammarExpr& grammar_expr) {
    return VisitElement(grammar_expr);
  }

  /*! \brief Visit a character class GrammarExpr. */
  virtual int32_t VisitCharacterClass(const GrammarExpr& grammar_expr) {
    return VisitElement(grammar_expr);
  }

  /*! \brief Visit a star quantifier GrammarExpr. */
  virtual int32_t VisitQuantifier(const GrammarExpr& grammar_expr) {
    int32_t new_id = VisitExpr(grammar_expr[0]);
    return builder_.AddQuantifier(new_id, grammar_expr.type);
  }

  virtual int32_t VisitQuantifierRange(const GrammarExpr& grammar_expr) {
    int32_t new_id = VisitExpr(grammar_expr[0]);
    return builder_.AddQuantifierRange(new_id, grammar_expr[1], grammar_expr[2]);
  }

  /*! \brief Visit a rule reference GrammarExpr. */
  virtual int32_t VisitRuleRef(const GrammarExpr& grammar_expr) {
    return VisitElement(grammar_expr);
  }

  /*! \brief The grammar to visit or mutate. */
  BNFGrammar grammar_;
  /*!
   * \brief The builder to build the new grammar. It is empty when the mutator is constructed,
   and
   * can be used to build a new grammar in subclasses.
   */
  BNFGrammarBuilder builder_;
  /*! \brief The name of the current rule being visited. */
  std::string cur_rule_name_;
};

/*!
 * \brief Normalize a BNFGrammar: expand the nested rules, combine consequent sequences and
 * strings, etc.
 */
class BNFGrammarNormalizer : public BNFGrammarMutator {
 public:
  using BNFGrammarMutator::BNFGrammarMutator;

  BNFGrammar Apply(const BNFGrammar& grammar) final;
};

}  // namespace xgrammar

#endif  // XGRAMMAR_GRAMMAR_FUNCTOR_H_
