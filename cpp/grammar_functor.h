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

namespace xgrammar {

/*!
 * \brief Base class for visitors and mutators of the BNF grammar.
 * \tparam T The type of the return value of visitor functions. Typical values:
 * - int32_t: the id of the new grammar_expr
 * - void: no return value
 * \tparam ReturnType The type of the return value of the transform function Apply(). Typical
 values
 * are void (for visitor) and BNFGrammar (for mutator).
 */
template <typename T = int32_t, typename ReturnType = BNFGrammar>
class BNFGrammarFunctor {
 public:
  /*!
   * \brief Constructor.
   * \param grammar The grammar to visit or mutate.
   */
  explicit BNFGrammarFunctor() {}

  /*!
   * \brief Apply the transformation to the grammar, or visit the grammar.
   * \return The transformed grammar, or the visiting result, or void.
   */
  virtual ReturnType Apply(const BNFGrammar& grammar) {
    Init(grammar);
    if constexpr (std::is_same<T, void>::value) {
      for (int i = 0; i < static_cast<int>(grammar_->NumRules()); ++i) {
        auto rule = grammar_->GetRule(i);
        cur_rule_name_ = rule.name;
        VisitExpr(rule.body_expr_id);
        VisitLookaheadAssertion(rule.lookahead_assertion_id);
      }
    } else if constexpr (std::is_same<T, int32_t>::value &&
                         std::is_same<ReturnType, BNFGrammar>::value) {
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
    } else {
      return ReturnType();
    }
  }

  /*! \brief Virtual destructor. */
  virtual ~BNFGrammarFunctor() = default;

 protected:
  using Rule = BNFGrammar::Impl::Rule;
  using GrammarExpr = BNFGrammar::Impl::GrammarExpr;
  using GrammarExprType = BNFGrammar::Impl::GrammarExprType;

  /*! \brief Initialize the functor. Should be called at the beginning of Apply(). */
  virtual void Init(const BNFGrammar& grammar) {
    grammar_ = grammar;
    builder_ = BNFGrammarBuilder();
  }

  /*! \brief Visit a lookahead assertion expr referred by id. */
  virtual T VisitLookaheadAssertion(int32_t lookahead_assertion_id) {
    if (lookahead_assertion_id == -1) {
      return -1;
    }
    return VisitExpr(lookahead_assertion_id);
  }

  /*! \brief Visit a GrammarExpr by id. */
  virtual T VisitExpr(int32_t old_grammar_expr_id) {
    return VisitExpr(grammar_->GetGrammarExpr(old_grammar_expr_id));
  }

  /*! \brief Visit a GrammarExpr. Dispatch to the corresponding Visit function. */
  virtual T VisitExpr(const GrammarExpr& grammar_expr) {
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
      case GrammarExprType::kCharacterClassStar:
        return VisitCharacterClassStar(grammar_expr);
      case GrammarExprType::kRuleRef:
        return VisitRuleRef(grammar_expr);
      default:
        XGRAMMAR_LOG(FATAL) << "Unexpected sequence type: " << static_cast<int>(grammar_expr.type);
    }
  }

  /*! \brief Visit a choices GrammarExpr. */
  virtual T VisitChoices(const GrammarExpr& grammar_expr) {
    if constexpr (std::is_same<T, void>::value) {
      for (auto i : grammar_expr) {
        VisitExpr(i);
      }
    } else if constexpr (std::is_same<T, int32_t>::value) {
      std::vector<int32_t> choice_ids;
      for (int32_t i : grammar_expr) {
        choice_ids.push_back(VisitExpr(i));
      }
      return builder_.AddChoices(choice_ids);
    } else {
      return T();
    }
  }

  /*! \brief Visit a sequence GrammarExpr. */
  virtual T VisitSequence(const GrammarExpr& grammar_expr) {
    if constexpr (std::is_same<T, void>::value) {
      for (auto i : grammar_expr) {
        VisitExpr(i);
      }
    } else if constexpr (std::is_same<T, int32_t>::value) {
      std::vector<T> sequence_ids;
      for (int32_t i : grammar_expr) {
        sequence_ids.push_back(VisitExpr(i));
      }
      return builder_.AddSequence(sequence_ids);
    } else {
      return T();
    }
  }

  /*! \brief Visit an element GrammarExpr, including empty string, character class, and rule ref.
   */
  virtual T VisitElement(const GrammarExpr& grammar_expr) {
    if constexpr (std::is_same<T, void>::value) {
      return;
    } else if constexpr (std::is_same<T, int32_t>::value) {
      return builder_.AddGrammarExpr(grammar_expr);
    } else {
      return T();
    }
  }

  /*! \brief Visit an empty string GrammarExpr. */
  virtual T VisitEmptyStr(const GrammarExpr& grammar_expr) { return VisitElement(grammar_expr); }

  /*! \brief Visit a character class GrammarExpr. */
  virtual T VisitByteString(const GrammarExpr& grammar_expr) { return VisitElement(grammar_expr); }

  /*! \brief Visit a character class GrammarExpr. */
  virtual T VisitCharacterClass(const GrammarExpr& grammar_expr) {
    return VisitElement(grammar_expr);
  }

  /*! \brief Visit a star quantifier GrammarExpr. */
  virtual T VisitCharacterClassStar(const GrammarExpr& grammar_expr) {
    return VisitElement(grammar_expr);
  }

  /*! \brief Visit a rule reference GrammarExpr. */
  virtual T VisitRuleRef(const GrammarExpr& grammar_expr) { return VisitElement(grammar_expr); }

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
 * \brief Visitor of BNFGrammar.
 * \tparam ReturnType The return type of the Apply() function. Denotes the collected information.
 */
template <typename ReturnType>
using BNFGrammarVisitor = BNFGrammarFunctor<void, ReturnType>;

/*!
 * \brief Mutator of BNFGrammar. The Apply() function returns the updated grammar.
 */
using BNFGrammarMutator = BNFGrammarFunctor<int32_t, BNFGrammar>;

/*!
 * \brief Normalize a BNFGrammar: expand the nested rules, combine consequent sequences and strings,
 * etc.
 */
class BNFGrammarNormalizer : public BNFGrammarMutator {
 public:
  using BNFGrammarMutator::BNFGrammarMutator;

  BNFGrammar Apply(const BNFGrammar& grammar) final;

 private:
  std::vector<std::unique_ptr<BNFGrammarMutator>> GetNormalizerList();
};

}  // namespace xgrammar

#endif  // XGRAMMAR_GRAMMAR_FUNCTOR_H_
