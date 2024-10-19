/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/grammar_serializer.h
 * \brief The header for printing the AST of a BNF grammar.
 */

#ifndef XGRAMMAR_GRAMMAR_SERIALIZER_H_
#define XGRAMMAR_GRAMMAR_SERIALIZER_H_

#include <xgrammar/xgrammar.h>

#include <string>

#include "grammar_ast.h"

namespace xgrammar {

/*!
 * \brief Serialize the abstract syntax tree of a BNF grammar to a string.
 */
class BNFGrammarSerializer {
 public:
  /*!
   * \brief Constructor.
   * \param grammar The grammar to print.
   */
  explicit BNFGrammarSerializer(const BNFGrammar& grammar) : grammar_(grammar) {}

  /*! \brief Serialize the grammar to string. */
  virtual std::string ToString() = 0;

 protected:
  const BNFGrammar& grammar_;
};

/*!
 * \brief Prints the BNF AST with standard BNF format.
 */
class BNFGrammarPrinter : public BNFGrammarSerializer {
 private:
  using Rule = BNFGrammar::Impl::Rule;
  using RuleExprType = BNFGrammar::Impl::RuleExprType;
  using RuleExpr = BNFGrammar::Impl::RuleExpr;

 public:
  /*!
   * \brief Constructor.
   * \param grammar The grammar to print.
   */
  explicit BNFGrammarPrinter(const BNFGrammar& grammar) : BNFGrammarSerializer(grammar) {}

  /*! \brief Print the complete grammar. */
  std::string ToString() final;

  /*! \brief Print a rule. */
  std::string PrintRule(const Rule& rule);
  /*! \brief Print a rule corresponding to the given id. */
  std::string PrintRule(int32_t rule_id);
  /*! \brief Print a RuleExpr. */
  std::string PrintRuleExpr(const RuleExpr& rule_expr);
  /*! \brief Print a RuleExpr corresponding to the given id. */
  std::string PrintRuleExpr(int32_t rule_expr_id);

 private:
  /*! \brief Print a RuleExpr for byte string. */
  std::string PrintByteString(const RuleExpr& rule_expr);
  /*! \brief Print a RuleExpr for character class. */
  std::string PrintCharacterClass(const RuleExpr& rule_expr);
  /*! \brief Print a RuleExpr for a star quantifier of a character class. */
  std::string PrintCharacterClassStar(const RuleExpr& rule_expr);
  /*! \brief Print a RuleExpr for empty string. */
  std::string PrintEmptyStr(const RuleExpr& rule_expr);
  /*! \brief Print a RuleExpr for rule reference. */
  std::string PrintRuleRef(const RuleExpr& rule_expr);
  /*! \brief Print a RuleExpr for rule_expr sequence. */
  std::string PrintSequence(const RuleExpr& rule_expr);
  /*! \brief Print a RuleExpr for rule_expr choices. */
  std::string PrintChoices(const RuleExpr& rule_expr);
};

/*!
 * \brief Serialize the raw representation of the BNF AST to a string with JSON format.
 * \sa BNFJSONParser::Parse for parsing the JSON string.
 * \details JSON format:
 *  {
 *    "rules": [
 *      {"name": "...", "rule_expr": rule_expr_id},
 *      {"name": "...", "rule_expr": rule_expr_id},
 *    ],
 *    "rule_expr_data": [integers...],
 *    "rule_expr_indptr": [integers...],
 *  }
 */
class BNFGrammarJSONSerializer : public BNFGrammarSerializer {
 public:
  /*!
   * \brief Constructor.
   * \param grammar The grammar to print.
   */
  explicit BNFGrammarJSONSerializer(const BNFGrammar& grammar, bool prettify = true)
      : BNFGrammarSerializer(grammar), prettify_(prettify) {}

  /*!
   * \brief Dump the raw representation of the AST to a JSON file.
   * \param prettify Whether to format the JSON string. If false, all whitespaces will be removed.
   */
  std::string ToString() final;

 private:
  bool prettify_;
};

/*!
 * \brief Parse a BNF grammar from the raw representation of the AST in JSON format.
 */
class BNFGrammarDeserializer {
 public:
  /*!
   * \brief Parse the JSON string
   * \param json_string The JSON string.
   * \return The parsed BNF grammar.
   */
  static BNFGrammar Deserialize(std::string json_string);
};

}  // namespace xgrammar

#endif  // XGRAMMAR_GRAMMAR_SERIALIZER_H_
