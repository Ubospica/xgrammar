/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/grammar.h
 * \brief The header for the support of grammar-guided generation.
 */

#ifndef XGRAMMAR_GRAMMAR_AST_H_
#define XGRAMMAR_GRAMMAR_AST_H_

#include <xgrammar/xgrammar.h>

#include <string>
#include <vector>

#include "support/csr_array.h"
#include "support/logging.h"

namespace xgrammar {

/*!
 * \brief This class stores the abstract syntax tree (AST) of the Backus-Naur Form (BNF) grammar.
 * The BNF definition here is standard BNF, and the characters are represented using regex-style
 * character classes (e.g. [a-z], [^a-z]).
 *
 * \details
 * ### Rules
 * The BNF grammar AST consists of a set of rules. Each rule contains a name and a definition, and
 * corresponds to a production in the grammar. The definition of a rule is a GrammarExpr. Each rule
 * has a rule_id for reference.
 *
 * ### GrammarExprs
 * GrammarExpr is the definition of a rule or part of the definition of a rule. It can contain
 * elements, empty string, reference to other GrammarExprs, or reference to other rules. Each
 * GrammarExpr corresponds to an grammar_expr_id for reference.
 *
 * For example, in the following rule: rule ::= ("a" "b") | "c"
 * ("a" "b"), "c", ("a" "b") | "c" are all GrammarExprs.
 *
 * #### Types of GrammarExprs
 * Every GrammarExpr is represented by a type as well as a variable-length array containing its
 * data. GrammarExpr has several types:
 * - Byte string: a string of bytes (0~255). Supports UTF-8 strings.
 * - Character class: a range of characters (each character is a unicode codepoint), e.g. [a-z],
 *   [ac-z]. Can be negated: [^a-z], [^ac-z]. Now only ascii chars is allowed in [], but this
 *   expression can accept/reject unicode chars.
 * - Character class star: a star quantifier of a character class. e.g. [a-z]*, [^a-z]*.
 * - EmptyStr: an empty string, i.e. ""
 * - Rule reference: a reference to another rule
 * - Sequence: a sequence of grammar_exprs, e.g. ("a" "b"). These grammar_exprs are concatenated
 * together.
 * - Choices: a choice of grammar_exprs, e.g. ("a" "b") | "c". Each grammar_expr can be matched.
 *
 * #### Storage of GrammarExprs
 * Each type of GrammarExpr has a different data format. For the format of each type of GrammarExpr,
 * see docs in BNFGrammar::Impl::GrammarExprType.
 *
 * We store all GrammarExprs in csr_matrix style. That is, they are stored consecutively in one
 * vector (data vector) and the starting position of each GrammarExpr is recorded in the indptr
 * vector.
 *
 * \remark The character class star GrammarExpr is for the special support for elements like [a-z]*
 * in the grammar. We add it to make the matching more efficient, as we can avoid recursion into
 * rules when matching a sequence of characters. It should be used like:
 * rule1 ::= ((element1 element2 rule2 ...) | ...)
 * rule2 ::= character_class_star_grammar_expr(id_of_a_character_class_grammar_expr)
 */
class BNFGrammar::Impl {
 public:
  /*! \brief A rule with name. */
  struct Rule {
    /*! \brief The name of the rule. */
    std::string name;
    /*! \brief The GrammarExpr id of the body of the rule. */
    int32_t body_expr_id;
    /*! \brief The id of the associated lookahead assertion expr. For now it must be a id of a
     * sequence GrammarExpr. -1 if not exists. */
    int32_t lookahead_assertion_id = -1;
  };

  /*! \brief Get the number of rules. */
  size_t NumRules() const { return rules_.size(); }
  /*! \brief Get the rule with the given id. */
  const Rule& GetRule(int32_t rule_id) const {
    XGRAMMAR_DCHECK(rule_id >= 0 && rule_id < static_cast<int32_t>(rules_.size()))
        << "rule_id " << rule_id << " is out of bound";
    return rules_[rule_id];
  }
  /*! \brief Get the root rule id of the grammar. */
  int32_t GetRootRuleId() const { return root_rule_id_; }
  /*! \brief Get the root rule of the grammar. */
  const Rule& GetRootRule() const {
    XGRAMMAR_DCHECK(root_rule_id_ >= 0 && root_rule_id_ < static_cast<int32_t>(rules_.size()))
        << "root_rule_id " << root_rule_id_ << " is out of bound";
    return rules_[root_rule_id_];
  }

  /*! \brief The type of the grammar expression. */
  enum class GrammarExprType : int32_t {
    // data format: [byte0, byte1, ...]
    kByteString,
    // data format: [is_negative, lower0, upper0, lower1, upper1, ...]
    kCharacterClass,
    // data format: []
    kEmptyStr,
    // data format: [rule_id]
    kRuleRef,
    // data format: [grammar_expr_id0, grammar_expr_id1, ...]
    kSequence,
    // data format: [grammar_expr_id0, grammar_expr_id1, ...]
    kChoices,
    // data format: [grammar_expr_id]
    kStarQuantifier,
    kPlusQuantifier,
    kQuestionQuantifier,
    // data format: [grammar_expr_id, lower, upper]
    kQuantifierRange,
  };

  /*! \brief The object representing a grammar expression. */
  struct GrammarExpr {
    /*! \brief The type of the grammar expression. */
    GrammarExprType type;
    /*! \brief The data of the GrammarExpr. A variable-length array. */
    const int32_t* data;
    /*! \brief The length of the data array. */
    int32_t data_len;

    int32_t size() const { return data_len; }
    /*! \brief Get the i-th element of the data array. */
    const int32_t& operator[](int i) const {
      XGRAMMAR_DCHECK(i >= 0 && i < size()) << "Index " << i << " is out of bound";
      return data[i];
    }
    const int32_t* begin() const { return data; }
    const int32_t* end() const { return data + data_len; }
  };

  /*! \brief Get the number of grammar_exprs. */
  int32_t NumGrammarExprs() const { return grammar_expr_data_.Size(); }
  /*! \brief Get the grammar_expr with the given id. */
  GrammarExpr GetGrammarExpr(int32_t grammar_expr_id) const {
    XGRAMMAR_DCHECK(grammar_expr_id >= 0 && grammar_expr_id < NumGrammarExprs())
        << "grammar_expr_id " << grammar_expr_id << " is out of bound";
    auto row = grammar_expr_data_[grammar_expr_id];
    return {static_cast<GrammarExprType>(row[0]), row.data + 1, row.data_len - 1};
  }

 private:
  /*! \brief The rules of the grammar. rule_id corresponds the index of this vector. */
  std::vector<Rule> rules_;
  CSRArray<int32_t> grammar_expr_data_;
  int32_t root_rule_id_ = -1;

  friend class BNFGrammarBuilder;
  friend class BNFGrammarJSONSerializer;
  friend class BNFGrammarDeserializer;
};

}  // namespace xgrammar

#endif  // XGRAMMAR_GRAMMAR_AST_H_
