/*!
 *  Copyright (c) 2024 by Contributors
 * \file xgrammar/grammar_serializer.cc
 */

#include "grammar_serializer.h"

#include <picojson.h>

#include "support/encoding.h"

namespace xgrammar {

std::string GrammarPrinter::PrintRule(const Rule& rule) {
  std::string res = rule.name + " ::= " + PrintRuleExpr(rule.body_expr_id);
  if (rule.lookahead_assertion_id != -1) {
    res += " (=" + PrintRuleExpr(rule.lookahead_assertion_id) + ")";
  }
  return res;
}

std::string GrammarPrinter::PrintRule(int32_t rule_id) {
  return PrintRule(grammar_->GetRule(rule_id));
}

std::string GrammarPrinter::PrintRuleExpr(const RuleExpr& rule_expr) {
  std::string result;
  switch (rule_expr.type) {
    case RuleExprType::kByteString:
      return PrintByteString(rule_expr);
    case RuleExprType::kCharacterClass:
      return PrintCharacterClass(rule_expr);
    case RuleExprType::kCharacterClassStar:
      return PrintCharacterClassStar(rule_expr);
    case RuleExprType::kEmptyStr:
      return PrintEmptyStr(rule_expr);
    case RuleExprType::kRuleRef:
      return PrintRuleRef(rule_expr);
    case RuleExprType::kSequence:
      return PrintSequence(rule_expr);
    case RuleExprType::kChoices:
      return PrintChoices(rule_expr);
    case RuleExprType::kTagDispatch:
      return PrintTagDispatch(rule_expr);
    default:
      XGRAMMAR_LOG(FATAL) << "Unexpected RuleExpr type: " << static_cast<int>(rule_expr.type);
      XGRAMMAR_UNREACHABLE();
  }
}

std::string GrammarPrinter::PrintRuleExpr(int32_t rule_expr_id) {
  return PrintRuleExpr(grammar_->GetRuleExpr(rule_expr_id));
}

std::string GrammarPrinter::PrintByteString(const RuleExpr& rule_expr) {
  std::string internal_str;
  internal_str.reserve(rule_expr.data_len);
  for (int i = 0; i < rule_expr.data_len; ++i) {
    internal_str += static_cast<char>(rule_expr[i]);
  }
  return "\"" + PrintAsEscapedUTF8(internal_str) + "\"";
}

std::string GrammarPrinter::PrintCharacterClass(const RuleExpr& rule_expr) {
  static const std::unordered_map<TCodepoint, std::string> kCustomEscapeMap = {
      {'-', "\\-"}, {']', "\\]"}
  };
  std::string result = "[";
  bool is_negative = static_cast<bool>(rule_expr[0]);
  if (is_negative) {
    result += "^";
  }
  for (auto i = 1; i < rule_expr.data_len; i += 2) {
    result += PrintAsEscapedUTF8(rule_expr[i], kCustomEscapeMap);
    if (rule_expr[i] == rule_expr[i + 1]) {
      continue;
    }
    result += "-";
    result += PrintAsEscapedUTF8(rule_expr[i + 1], kCustomEscapeMap);
  }
  result += "]";
  return result;
}

std::string GrammarPrinter::PrintCharacterClassStar(const RuleExpr& rule_expr) {
  return PrintCharacterClass(rule_expr) + "*";
}

std::string GrammarPrinter::PrintEmptyStr(const RuleExpr& rule_expr) { return "\"\""; }

std::string GrammarPrinter::PrintRuleRef(const RuleExpr& rule_expr) {
  return grammar_->GetRule(rule_expr[0]).name;
}

std::string GrammarPrinter::PrintSequence(const RuleExpr& rule_expr) {
  std::string result;
  result += "(";
  for (int i = 0; i < rule_expr.data_len; ++i) {
    result += PrintRuleExpr(rule_expr[i]);
    if (i + 1 != rule_expr.data_len) {
      result += " ";
    }
  }
  result += ")";
  return result;
}

std::string GrammarPrinter::PrintChoices(const RuleExpr& rule_expr) {
  std::string result;

  result += "(";
  for (int i = 0; i < rule_expr.data_len; ++i) {
    result += PrintRuleExpr(rule_expr[i]);
    if (i + 1 != rule_expr.data_len) {
      result += " | ";
    }
  }
  result += ")";
  return result;
}

std::string GrammarPrinter::PrintString(const std::string& str) {
  return "\"" + PrintAsEscapedUTF8(str) + "\"";
}

std::string GrammarPrinter::PrintBoolean(bool value) { return value ? "true" : "false"; }

std::string GrammarPrinter::PrintTagDispatch(const RuleExpr& rule_expr) {
  auto tag_dispatch = grammar_->GetTagDispatch(rule_expr);
  std::string result = "TagDispatch(\n";
  std::string indent = "  ";
  for (const auto& [tag, rule_id] : tag_dispatch.tag_rule_pairs) {
    result += indent + "(" + PrintString(tag) + ", " + grammar_->GetRule(rule_id).name + "),\n";
  }
  result += indent + "stop_eos=" + PrintBoolean(tag_dispatch.stop_eos) + ",\n";
  result += indent + "stop_str=(";
  for (int i = 0; i < static_cast<int>(tag_dispatch.stop_str.size()); ++i) {
    result += PrintString(tag_dispatch.stop_str[i]);
    if (i + 1 != static_cast<int>(tag_dispatch.stop_str.size())) {
      result += ", ";
    }
  }
  result += "),\n";
  result += indent + "loop_after_dispatch=" + PrintBoolean(tag_dispatch.loop_after_dispatch) + "\n";
  result += ")";
  return result;
}

std::string GrammarPrinter::ToString() {
  std::string result;
  int num_rules = grammar_->NumRules();
  for (auto i = 0; i < num_rules; ++i) {
    result += PrintRule(grammar_->GetRule(i)) + "\n";
  }
  return result;
}

}  // namespace xgrammar
