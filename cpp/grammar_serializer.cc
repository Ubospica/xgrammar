/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/grammar_serializer.cc
 */

#include "grammar_serializer.h"

#include <picojson.h>

#include "support/encoding.h"

namespace xgrammar {

std::string BNFGrammarPrinter::PrintRule(const Rule& rule) {
  std::string res = rule.name + " ::= " + PrintGrammarExpr(rule.body_expr_id);
  if (rule.lookahead_assertion_id != -1) {
    res += " (=" + PrintGrammarExpr(rule.lookahead_assertion_id) + ")";
  }
  return res;
}

std::string BNFGrammarPrinter::PrintRule(int32_t rule_id) {
  return PrintRule(grammar_->GetRule(rule_id));
}

std::string BNFGrammarPrinter::PrintGrammarExpr(const GrammarExpr& grammar_expr) {
  std::string result;
  switch (grammar_expr.type) {
    case GrammarExprType::kByteString:
      return PrintByteString(grammar_expr);
    case GrammarExprType::kCharacterClass:
      return PrintCharacterClass(grammar_expr);
    case GrammarExprType::kStarQuantifier:
    case GrammarExprType::kPlusQuantifier:
    case GrammarExprType::kQuestionQuantifier:
      return PrintQuantifier(grammar_expr);
    case GrammarExprType::kQuantifierRange:
      return PrintQuantifierRange(grammar_expr);
    case GrammarExprType::kEmptyStr:
      return PrintEmptyStr(grammar_expr);
    case GrammarExprType::kRuleRef:
      return PrintRuleRef(grammar_expr);
    case GrammarExprType::kSequence:
      return PrintSequence(grammar_expr);
    case GrammarExprType::kChoices:
      return PrintChoices(grammar_expr);
    default:
      XGRAMMAR_LOG(FATAL) << "Unexpected GrammarExpr type: " << static_cast<int>(grammar_expr.type);
  }
}

std::string BNFGrammarPrinter::PrintGrammarExpr(int32_t grammar_expr_id) {
  return PrintGrammarExpr(grammar_->GetGrammarExpr(grammar_expr_id));
}

std::string BNFGrammarPrinter::PrintByteString(const GrammarExpr& grammar_expr) {
  std::string internal_str;
  internal_str.reserve(grammar_expr.data_len);
  for (int i = 0; i < grammar_expr.data_len; ++i) {
    internal_str += static_cast<char>(grammar_expr[i]);
  }
  auto codepoints = ParseUTF8(internal_str.c_str(), UTF8ErrorPolicy::kReturnByte);
  std::string result;
  for (auto codepoint : codepoints) {
    result += PrintAsEscapedUTF8(codepoint);
  }
  return "\"" + result + "\"";
}

std::string BNFGrammarPrinter::PrintCharacterClass(const GrammarExpr& grammar_expr) {
  static const std::unordered_map<TCodepoint, std::string> kCustomEscapeMap = {
      {'-', "\\-"}, {']', "\\]"}
  };
  std::string result = "[";
  bool is_negative = static_cast<bool>(grammar_expr[0]);
  if (is_negative) {
    result += "^";
  }
  for (auto i = 1; i < grammar_expr.data_len; i += 2) {
    result += PrintAsEscapedUTF8(grammar_expr[i], kCustomEscapeMap);
    if (grammar_expr[i] == grammar_expr[i + 1]) {
      continue;
    }
    result += "-";
    result += PrintAsEscapedUTF8(grammar_expr[i + 1], kCustomEscapeMap);
  }
  result += "]";
  return result;
}

std::string BNFGrammarPrinter::PrintQuantifier(const GrammarExpr& grammar_expr) {
  char quantifier;
  switch (grammar_expr.type) {
    case GrammarExprType::kStarQuantifier:
      quantifier = '*';
      break;
    case GrammarExprType::kPlusQuantifier:
      quantifier = '+';
      break;
    case GrammarExprType::kQuestionQuantifier:
      quantifier = '?';
      break;
    default:
      XGRAMMAR_LOG(FATAL) << "Unexpected GrammarExpr type: " << static_cast<int>(grammar_expr.type);
  }
  return PrintGrammarExpr(grammar_expr[0]) + quantifier;
}

std::string BNFGrammarPrinter::PrintQuantifierRange(const GrammarExpr& grammar_expr) {
  std::string lower = std::to_string(grammar_expr[1]);
  std::string upper = grammar_expr[2] == -1 ? "" : std::to_string(grammar_expr[2]);
  return PrintGrammarExpr(grammar_expr[0]) + "{" + lower + "," + upper + "}";
}

std::string BNFGrammarPrinter::PrintEmptyStr(const GrammarExpr& grammar_expr) { return "\"\""; }

std::string BNFGrammarPrinter::PrintRuleRef(const GrammarExpr& grammar_expr) {
  return grammar_->GetRule(grammar_expr[0]).name;
}

std::string BNFGrammarPrinter::PrintSequence(const GrammarExpr& grammar_expr) {
  std::string result;
  result += "(";
  for (int i = 0; i < grammar_expr.data_len; ++i) {
    result += PrintGrammarExpr(grammar_expr[i]);
    if (i + 1 != grammar_expr.data_len) {
      result += " ";
    }
  }
  result += ")";
  return result;
}

std::string BNFGrammarPrinter::PrintChoices(const GrammarExpr& grammar_expr) {
  std::string result;

  result += "(";
  for (int i = 0; i < grammar_expr.data_len; ++i) {
    result += PrintGrammarExpr(grammar_expr[i]);
    if (i + 1 != grammar_expr.data_len) {
      result += " | ";
    }
  }
  result += ")";
  return result;
}

std::string BNFGrammarPrinter::ToString() {
  std::string result;
  int num_rules = grammar_->NumRules();
  for (auto i = 0; i < num_rules; ++i) {
    result += PrintRule(grammar_->GetRule(i)) + "\n";
  }
  return result;
}

std::string BNFGrammarJSONSerializer::Serialize() {
  picojson::object grammar_json_obj;

  picojson::array rules_json;
  for (const auto& rule : grammar_->rules_) {
    picojson::object rule_json;
    rule_json["name"] = picojson::value(rule.name);
    rule_json["body_expr_id"] = picojson::value(static_cast<int64_t>(rule.body_expr_id));
    rules_json.push_back(picojson::value(rule_json));
  }
  grammar_json_obj["rules"] = picojson::value(rules_json);

  grammar_json_obj["grammar_expr_data"] = grammar_->grammar_expr_data_.Serialize();

  auto grammar_json = picojson::value(grammar_json_obj);
  return grammar_json.serialize(prettify_);
}

BNFGrammar BNFGrammarDeserializer::Deserialize(std::string json_string) {
  auto node = std::make_shared<BNFGrammar::Impl>();

  picojson::value serialized_value;
  std::string err = picojson::parse(serialized_value, json_string);

  XGRAMMAR_CHECK(err.empty() && serialized_value.is<picojson::object>())
      << "Failed to parse JSON object from string: " << json_string;
  auto serialized_obj = serialized_value.get<picojson::object>();

  // rules
  XGRAMMAR_CHECK(serialized_obj.count("rules") && serialized_obj["rules"].is<picojson::array>())
      << "Missing or invalid 'rules' field in JSON object";
  auto rules_array = serialized_obj["rules"].get<picojson::array>();

  XGRAMMAR_CHECK(rules_array.size() > 0) << "Rules array is empty";
  for (const auto& rule_value : rules_array) {
    XGRAMMAR_CHECK(rule_value.is<picojson::object>()) << "Invalid rule object in rules array";
    auto rule_obj = rule_value.get<picojson::object>();
    XGRAMMAR_CHECK(rule_obj.count("name") && rule_obj["name"].is<std::string>())
        << "Missing or invalid 'name' field in rule object";
    auto name = rule_obj["name"].get<std::string>();
    XGRAMMAR_CHECK(rule_obj.count("body_expr_id") && rule_obj["body_expr_id"].is<int64_t>())
        << "Missing or invalid 'body_expr_id' field in rule object";
    auto grammar_expr = static_cast<int32_t>(rule_obj["body_expr_id"].get<int64_t>());
    node->rules_.push_back(BNFGrammar::Impl::Rule({name, grammar_expr}));
  }

  // grammar_expr_data
  XGRAMMAR_CHECK(serialized_obj.count("grammar_expr_data"))
      << "Missing or invalid 'grammar_expr_data' field in JSON object";
  node->grammar_expr_data_ = CSRArray<int32_t>::Deserialize(serialized_obj["grammar_expr_data"]);

  return BNFGrammar(node);
}

}  // namespace xgrammar
