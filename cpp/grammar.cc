/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/grammar.cc
 */

#include <xgrammar/grammar.h>

#include "grammar_data_structure.h"
#include "grammar_functor.h"
#include "grammar_parser.h"
#include "grammar_serializer.h"
#include "json_schema_converter.h"
#include "regex_converter.h"
#include "structural_tag.h"

namespace xgrammar {

std::string Grammar::ToString() const { return GrammarPrinter(*this).ToString(); }

Grammar Grammar::FromEBNF(const std::string& ebnf_string, const std::string& root_rule_name) {
  auto grammar = ParseEBNF(ebnf_string, root_rule_name);
  grammar = GrammarNormalizer().Apply(grammar);
  return grammar;
}

Grammar Grammar::FromJSONSchema(
    const std::string& schema,
    bool any_whitespace,
    std::optional<int> indent,
    std::optional<std::pair<std::string, std::string>> separators,
    bool strict_mode
) {
  auto ebnf_string = JSONSchemaToEBNF(schema, any_whitespace, indent, separators, strict_mode);
  return FromEBNF(ebnf_string);
}

Grammar Grammar::FromRegex(const std::string& regex) { return FromEBNF(RegexToEBNF(regex)); }

Grammar Grammar::FromStructuralTag(
    const std::vector<StructuralTagItem>& tags, const std::vector<std::string>& triggers
) {
  return StructuralTagToGrammar(tags, triggers);
}

// Optimized json grammar for the speed of the grammar matcher
const std::string kJSONGrammarString = R"(
root ::= (
    "{" space members_and_embrace |
    "[" space elements_or_embrace
)
value_non_str ::= (
    "{" space members_and_embrace |
    "[" space elements_or_embrace |
    "0" fraction exponent |
    [1-9] [0-9]* fraction exponent |
    "-" [0-9] fraction exponent |
    "-" [1-9] [0-9]* fraction exponent |
    "true" |
    "false" |
    "null"
)
members_and_embrace ::= ("\"" ([^"\\\x00-\x1F] | "\\" escape)* "\"" space ":" space members_suffix | "}")
members_suffix ::= (
    value_non_str space member_suffix_suffix |
    "\"" ([^"\\\x00-\x1F] | "\\" escape)* "\"" space "}" |
    "\"" ([^"\\\x00-\x1F] | "\\" escape)* "\"" space "," space "\"" ([^"\\\x00-\x1F] | "\\" escape)* "\"" space ":" space members_suffix
)
member_suffix_suffix ::= (
    "}" |
    "," space "\"" ([^"\\\x00-\x1F] | "\\" escape)* "\"" space ":" space members_suffix
)
elements_or_embrace ::= (
    "{" space members_and_embrace elements_rest space "]" |
    "[" space elements_or_embrace elements_rest space "]" |
    "\"" ([^"\\\x00-\x1F] | "\\" escape)* "\"" elements_rest space "]" |
    "0" fraction exponent elements_rest space "]" |
    [1-9] [0-9]* fraction exponent elements_rest space "]" |
    "-" "0" fraction exponent elements_rest space "]" |
    "-" [1-9] [0-9]* fraction exponent elements_rest space "]" |
    "true" elements_rest space "]" |
    "false" elements_rest space "]" |
    "null" elements_rest space "]" |
    "]"
)
elements ::= (
    "{" space members_and_embrace elements_rest |
    "[" space elements_or_embrace elements_rest |
    "\"" ([^"\\\x00-\x1F] | "\\" escape)* "\"" elements_rest |
    "0" fraction exponent elements_rest |
    [1-9] [0-9]* fraction exponent elements_rest |
    "-" [0-9] fraction exponent elements_rest |
    "-" [1-9] [0-9]* fraction exponent elements_rest |
    "true" elements_rest |
    "false" elements_rest |
    "null" elements_rest
)
elements_rest ::= (
    "" |
    space "," space elements
)
space ::= [ \n\t]*
escape ::= ["\\/bfnrt] | "u" [A-Fa-f0-9] [A-Fa-f0-9] [A-Fa-f0-9] [A-Fa-f0-9]
fraction ::= "" | "." number
exponent ::= "" |  "e" sign number | "E" sign number
number ::= [0-9] [0-9]*
sign ::= "" | "+" | "-"
)";

Grammar Grammar::BuiltinJSONGrammar() {
  static const Grammar grammar = FromEBNF(kJSONGrammarString);
  return grammar;
}

Grammar Grammar::Union(const std::vector<Grammar>& grammars) {
  return GrammarUnionFunctor::Apply(grammars);
}

Grammar Grammar::Concat(const std::vector<Grammar>& grammars) {
  return GrammarConcatFunctor::Apply(grammars);
}

std::ostream& operator<<(std::ostream& os, const Grammar& grammar) {
  os << grammar.ToString();
  return os;
}

}  // namespace xgrammar
