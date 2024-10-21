/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/grammar_serializer.h
 * \brief The header for printing the AST of a BNF grammar.
 */

#ifndef XGRAMMAR_GRAMMAR_SERIALIZER_H_
#define XGRAMMAR_GRAMMAR_SERIALIZER_H_

#include <xgrammar/xgrammar.h>

#include <string>

#include "grammar_data_structure.h"

namespace xgrammar {

/*!
 * \brief Prints the BNF AST with standard BNF format.
 */
class BNFGrammarPrinter {
 private:
  using Rule = BNFGrammar::Impl::Rule;
  using GrammarExprType = BNFGrammar::Impl::GrammarExprType;
  using GrammarExpr = BNFGrammar::Impl::GrammarExpr;

 public:
  /*!
   * \brief Constructor.
   * \param grammar The grammar to print.
   */
  explicit BNFGrammarPrinter(const BNFGrammar& grammar) : grammar_(grammar) {}

  /*! \brief Print the complete grammar. */
  std::string ToString();

  /*! \brief Print a rule. */
  std::string PrintRule(const Rule& rule);
  /*! \brief Print a rule corresponding to the given id. */
  std::string PrintRule(int32_t rule_id);
  /*! \brief Print a GrammarExpr. */
  std::string PrintGrammarExpr(const GrammarExpr& grammar_expr);
  /*! \brief Print a GrammarExpr corresponding to the given id. */
  std::string PrintGrammarExpr(int32_t grammar_expr_id);

 private:
  /*! \brief Print a GrammarExpr for byte string. */
  std::string PrintByteString(const GrammarExpr& grammar_expr);
  /*! \brief Print a GrammarExpr for character class. */
  std::string PrintCharacterClass(const GrammarExpr& grammar_expr);
  /*! \brief Print a GrammarExpr for a quantifier. */
  std::string PrintQuantifier(const GrammarExpr& grammar_expr);
  /*! \brief Print a GrammarExpr for a quantifier range. */
  std::string PrintQuantifierRange(const GrammarExpr& grammar_expr);
  /*! \brief Print a GrammarExpr for empty string. */
  std::string PrintEmptyStr(const GrammarExpr& grammar_expr);
  /*! \brief Print a GrammarExpr for rule reference. */
  std::string PrintRuleRef(const GrammarExpr& grammar_expr);
  /*! \brief Print a GrammarExpr for grammar_expr sequence. */
  std::string PrintSequence(const GrammarExpr& grammar_expr);
  /*! \brief Print a GrammarExpr for grammar_expr choices. */
  std::string PrintChoices(const GrammarExpr& grammar_expr);

  BNFGrammar grammar_;
};

/*!
 * \brief Serialize the raw representation of the BNF AST to a string with JSON format.
 * \sa BNFJSONParser::Parse for parsing the JSON string.
 * \details JSON format:
 *  {
 *    "rules": [
 *      {"name": "...", "grammar_expr": grammar_expr_id},
 *      {"name": "...", "grammar_expr": grammar_expr_id},
 *    ],
 *    "grammar_expr_data": [integers...],
 *    "grammar_expr_indptr": [integers...],
 *  }
 */
class BNFGrammarJSONSerializer {
 public:
  /*!
   * \brief Constructor.
   * \param grammar The grammar to print.
   */
  explicit BNFGrammarJSONSerializer(const BNFGrammar& grammar, bool prettify = true)
      : prettify_(prettify) {}

  /*!
   * \brief Dump the raw representation of the AST to a JSON file.
   * \param prettify Whether to format the JSON string. If false, all whitespaces will be removed.
   */
  std::string Serialize();

 private:
  BNFGrammar grammar_;
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
