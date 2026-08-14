/**
 * \file tests/cpp/test_regex_fsm_cache.cc
 * \brief Regression tests for regex FSM reuse across JSON Schema compilation stages.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>

#include "fsm_builder.h"
#include "grammar_functor.h"
#include "json_schema_converter.h"
#include "regex_fsm_cache.h"
#include "xgrammar/xgrammar.h"

using namespace xgrammar;

TEST(XGrammarRegexFSMCacheTest, JSONSchemaConversionCacheSurvivesOptimization) {
  const std::string pattern = "[a-z]{1,3}";
  RegexFSMCache regex_fsm_cache;
  auto grammar = GrammarNormalizer::Apply(JSONSchemaToGrammar(
      R"({"type":"string","pattern":"[a-z]{1,3}"})",
      /*any_whitespace=*/false,
      /*indent=*/std::nullopt,
      /*separators=*/std::nullopt,
      /*strict_mode=*/true,
      /*max_whitespace_cnt=*/std::nullopt,
      /*any_order=*/false,
      JSONFormat::kJSON,
      &regex_fsm_cache
  ));

  // JSON Schema search semantics rewrite the source pattern before compiling it. Preserve and
  // track the resulting cache entry rather than assuming the source pattern itself is the key.
  ASSERT_EQ(regex_fsm_cache.size(), 1);
  auto cached = regex_fsm_cache.begin();
  const std::string cache_key = cached->first;
  const std::size_t cache_size = regex_fsm_cache.size();
  const auto* cached_fsm_impl = cached->second.GetFsm().ImplPtr();

  auto optimized =
      GrammarOptimizer::Apply(grammar, /*expand_repetition_ranges=*/false, &regex_fsm_cache);

  EXPECT_TRUE(optimized->optimized);
  EXPECT_GT(optimized->complete_fsm.NumStates(), 0);
  EXPECT_EQ(regex_fsm_cache.size(), cache_size);
  cached = regex_fsm_cache.find(cache_key);
  ASSERT_NE(cached, regex_fsm_cache.end());
  EXPECT_EQ(cached->second.GetFsm().ImplPtr(), cached_fsm_impl);
}

TEST(XGrammarRegexFSMCacheTest, JSONSchemaSearchPatternIsDeterminized) {
  RegexFSMCache regex_fsm_cache;
  JSONSchemaToGrammar(
      R"({"type":"string","pattern":"abc"})",
      /*any_whitespace=*/false,
      /*indent=*/std::nullopt,
      /*separators=*/std::nullopt,
      /*strict_mode=*/true,
      /*max_whitespace_cnt=*/std::nullopt,
      /*any_order=*/false,
      JSONFormat::kJSON,
      &regex_fsm_cache
  );

  ASSERT_EQ(regex_fsm_cache.size(), 1);
  EXPECT_TRUE(regex_fsm_cache.begin()->second.IsDFA());
}

TEST(XGrammarRegexFSMCacheTest, JSONSchemaAlternativesShareSearchWildcards) {
  RegexFSMCache regex_fsm_cache;
  JSONSchemaToGrammar(
      R"({"type":"string","pattern":"Red|Blue|Yellow|Gold|Silver|Crystal"})",
      /*any_whitespace=*/false,
      /*indent=*/std::nullopt,
      /*separators=*/std::nullopt,
      /*strict_mode=*/true,
      /*max_whitespace_cnt=*/std::nullopt,
      /*any_order=*/false,
      JSONFormat::kJSON,
      &regex_fsm_cache
  );

  ASSERT_EQ(regex_fsm_cache.size(), 1);
  // The search wildcard pair is shared across all alternatives. The previous branch-wise rewrite
  // exceeds the 4096-state determinization limit for these six short literals, while the grouped
  // form stays below 400 states.
  EXPECT_LT(regex_fsm_cache.begin()->second.NumStates(), 400);
  EXPECT_TRUE(regex_fsm_cache.begin()->second.IsDFA());
}

TEST(XGrammarRegexFSMCacheTest, ASCIILiteralSearchFSMHandlesOverlappingPrefixesAndSuffixes) {
  auto result = BuildJSONSchemaPatternFSM(R"(bcd|abce|he|she|his|hers|a\.b|x\-y|c\/d|p\+q)", 4096);
  ASSERT_TRUE(result.IsOk());
  auto fsm = std::move(result).Unwrap();
  EXPECT_TRUE(fsm.IsDFA());
  EXPECT_TRUE(fsm.AcceptString("abcd"));    // bcd starts inside the failed abce branch.
  EXPECT_TRUE(fsm.AcceptString("ushers"));  // she and hers overlap through failure links.
  EXPECT_TRUE(fsm.AcceptString("prefix-a.b-suffix"));
  EXPECT_TRUE(fsm.AcceptString("prefix-x-y-suffix"));
  EXPECT_TRUE(fsm.AcceptString("prefix-c/d-suffix"));
  EXPECT_TRUE(fsm.AcceptString("prefix-p+q-suffix"));
  EXPECT_FALSE(fsm.AcceptString("abcf"));
  EXPECT_FALSE(fsm.AcceptString("prefix-aXb-suffix"));
}

TEST(XGrammarRegexFSMCacheTest, DecodedPatternFSMCountsUnicodeCodepoints) {
  auto one_result = BuildJSONSchemaPatternFSM("^.$", 4096);
  ASSERT_TRUE(one_result.IsOk());
  auto one = std::move(one_result).Unwrap();
  EXPECT_TRUE(one.IsDFA());
  EXPECT_TRUE(one.AcceptString("é"));
  EXPECT_TRUE(one.AcceptString("😀"));
  EXPECT_FALSE(one.AcceptString("éa"));

  auto two_result = BuildJSONSchemaPatternFSM("^..$", 4096);
  ASSERT_TRUE(two_result.IsOk());
  auto two = std::move(two_result).Unwrap();
  EXPECT_TRUE(two.AcceptString("éa"));
  EXPECT_TRUE(two.AcceptString("😀é"));
  EXPECT_FALSE(two.AcceptString("é"));

  auto class_result = BuildJSONSchemaPatternFSM("^[éê]$", 4096);
  ASSERT_TRUE(class_result.IsOk());
  auto character_class = std::move(class_result).Unwrap();
  EXPECT_TRUE(character_class.AcceptString("é"));
  EXPECT_TRUE(character_class.AcceptString("ê"));
  EXPECT_FALSE(character_class.AcceptString("a"));
}

TEST(XGrammarRegexFSMCacheTest, OptionalWideCharacterClassDoesNotBecomeNullable) {
  const std::string rewritten = RewriteJSONSchemaPatternForFullMatch(R"(^a(:[^x]+)?$)");
  auto nfa_result = RegexFSMBuilder::Build(rewritten);
  ASSERT_TRUE(nfa_result.IsOk());
  auto nfa = std::move(nfa_result).Unwrap();
  EXPECT_FALSE(nfa.AcceptString("a:")) << rewritten << "\n" << nfa;

  auto result = BuildJSONSchemaPatternFSM(R"(^a(:[^x]+)?$)", 4096);
  ASSERT_TRUE(result.IsOk());
  auto fsm = std::move(result).Unwrap();
  EXPECT_TRUE(fsm.IsDFA());
  EXPECT_TRUE(fsm.AcceptString("a"));
  EXPECT_TRUE(fsm.AcceptString("a:b"));
  EXPECT_FALSE(fsm.AcceptString("a:"));

  auto maskbench_result = BuildJSONSchemaPatternFSM(R"(^[^:\s]+:[^:\s]+(:[^\s]+)?$)", 4096);
  ASSERT_TRUE(maskbench_result.IsOk());
  auto maskbench_fsm = std::move(maskbench_result).Unwrap();
  EXPECT_TRUE(maskbench_fsm.AcceptString("chrome:latest"));
  EXPECT_TRUE(maskbench_fsm.AcceptString("chrome:latest:stable"));
  EXPECT_FALSE(maskbench_fsm.AcceptString("chrome:latest:"));

  auto locale_result = BuildJSONSchemaPatternFSM(
      R"(^[a-zA-Z]{2,3}(-[a-zA-Z]{4})?(-([a-zA-Z]{2}|[0-9]{3}))?(-[a-zA-Z]{5,8})?(-x(-[a-zA-Z0-9]{1,8})+)?$)",
      4096
  );
  ASSERT_TRUE(locale_result.IsOk());
  auto locale_fsm = std::move(locale_result).Unwrap();
  EXPECT_TRUE(locale_fsm.AcceptString("en-US"));
  EXPECT_TRUE(locale_fsm.AcceptString("fr"));
  EXPECT_FALSE(locale_fsm.AcceptString("invalid"));
}

TEST(XGrammarRegexFSMCacheTest, PatternLengthAvoidsProductFSM) {
  RegexFSMCache regex_fsm_cache;
  auto grammar = GrammarNormalizer::Apply(JSONSchemaToGrammar(
      R"({"type":"string","pattern":"[0-9]+","minLength":10,"maxLength":10})",
      /*any_whitespace=*/false,
      /*indent=*/std::nullopt,
      /*separators=*/std::nullopt,
      /*strict_mode=*/true,
      /*max_whitespace_cnt=*/std::nullopt,
      /*any_order=*/false,
      JSONFormat::kJSON,
      &regex_fsm_cache
  ));

  ASSERT_EQ(regex_fsm_cache.size(), 1);
  auto& entry = *regex_fsm_cache.begin();
  ASSERT_FALSE(entry.first.empty());
  EXPECT_FALSE(IsInternalRegexFSMCachePattern(entry.first.substr(1)));
  EXPECT_TRUE(entry.second.IsDFA());

  int32_t runtime_length_rule_count = 0;
  for (int32_t rule_id = 0; rule_id < grammar->NumRules(); ++rule_id) {
    const auto& rule = grammar->GetRule(rule_id);
    if (rule.json_string_min_chars >= 0) {
      ++runtime_length_rule_count;
      EXPECT_EQ(rule.json_string_min_chars, 10);
      EXPECT_EQ(rule.json_string_max_chars, 10);
    }
  }
  EXPECT_EQ(runtime_length_rule_count, 1);

  auto optimized =
      GrammarOptimizer::Apply(grammar, /*expand_repetition_ranges=*/false, &regex_fsm_cache);
  EXPECT_TRUE(optimized->optimized);
  EXPECT_GT(optimized->complete_fsm.NumStates(), 0);
}

TEST(XGrammarRegexFSMCacheTest, ExactLengthCharacterClassSearchAvoidsProductFSM) {
  RegexFSMCache regex_fsm_cache;
  JSONSchemaToGrammar(
      R"({"type":"string","pattern":"[0-9]{10,10}","minLength":10,"maxLength":10})",
      /*any_whitespace=*/false,
      /*indent=*/std::nullopt,
      /*separators=*/std::nullopt,
      /*strict_mode=*/true,
      /*max_whitespace_cnt=*/std::nullopt,
      /*any_order=*/false,
      JSONFormat::kJSON,
      &regex_fsm_cache
  );

  EXPECT_TRUE(regex_fsm_cache.empty());
}

TEST(XGrammarRegexFSMCacheTest, JSONPatternRepeatRetainsRawCharacterClassFastPath) {
  TokenizerInfo tokenizer_info(
      {"\"", "alpha_42", "alpha-42", "\\u0061", "!"},
      VocabType::RAW,
      std::nullopt,
      std::vector<int32_t>{}
  );
  GrammarCompiler compiler(
      tokenizer_info,
      /*max_threads=*/1,
      /*cache_enabled=*/true,
      /*max_memory_bytes=*/-1,
      /*enable_dynamic_compilation=*/true
  );
  auto compiled = compiler.CompileJSONSchema(
      R"({"type":"string","pattern":"^[A-Za-z0-9_-]{1,255}$"})",
      /*any_whitespace=*/false
  );
  GrammarMatcher matcher(
      compiled,
      /*override_stop_tokens=*/std::nullopt,
      /*terminate_without_stop_token=*/true
  );

  EXPECT_TRUE(matcher.AcceptToken(0));
  std::array<int32_t, 1> mask{-1};
  DLTensor tensor{};
  tensor.data = mask.data();
  tensor.device = DLDevice{kDLCPU, 0};
  tensor.ndim = 1;
  tensor.dtype = DLDataType{kDLInt, 32, 1};
  int64_t shape = 1;
  tensor.shape = &shape;
  EXPECT_TRUE(matcher.FillNextTokenBitmask(&tensor));
  EXPECT_TRUE((mask[0] & (1 << 1)) != 0);
  EXPECT_TRUE((mask[0] & (1 << 2)) != 0);
  EXPECT_TRUE((mask[0] & (1 << 3)) != 0);
  EXPECT_TRUE((mask[0] & (1 << 4)) == 0);
}

TEST(XGrammarRegexFSMCacheTest, JSONNumberMultipleOfMetadataSurvivesOptimization) {
  Grammar grammar =
      Grammar::FromJSONSchema(R"({"type":"number","minimum":-20,"maximum":20,"multipleOf":0.25})");
  int32_t constrained_rule = -1;
  for (int32_t rule_id = 0; rule_id < grammar->NumRules(); ++rule_id) {
    if (grammar->GetRule(rule_id).json_number_multiple_of_coefficient > 0) {
      constrained_rule = rule_id;
      break;
    }
  }
  ASSERT_GE(constrained_rule, 0);
  EXPECT_EQ(grammar->GetRule(constrained_rule).json_number_multiple_of_coefficient, 25);
  EXPECT_EQ(grammar->GetRule(constrained_rule).json_number_multiple_of_decimal_scale, 2);

  Grammar expanded = RepetitionRangeExpander::Apply(grammar);
  constrained_rule = -1;
  for (int32_t rule_id = 0; rule_id < expanded->NumRules(); ++rule_id) {
    if (expanded->GetRule(rule_id).json_number_multiple_of_coefficient > 0) {
      constrained_rule = rule_id;
      break;
    }
  }
  ASSERT_GE(constrained_rule, 0);
  EXPECT_EQ(expanded->GetRule(constrained_rule).json_number_multiple_of_coefficient, 25);

  Grammar optimized = GrammarOptimizer::Apply(grammar);
  constrained_rule = -1;
  for (int32_t rule_id = 0; rule_id < optimized->NumRules(); ++rule_id) {
    if (optimized->GetRule(rule_id).json_number_multiple_of_coefficient > 0) {
      constrained_rule = rule_id;
      break;
    }
  }
  ASSERT_GE(constrained_rule, 0);
  EXPECT_EQ(optimized->GetRule(constrained_rule).json_number_multiple_of_coefficient, 25);
  EXPECT_EQ(optimized->GetRule(constrained_rule).json_number_multiple_of_decimal_scale, 2);

  TokenizerInfo tokenizer_info(std::vector<std::string>{});
  for (bool cache_enabled : {false, true}) {
    GrammarCompiler compiler(tokenizer_info, /*max_threads=*/1, cache_enabled);
    CompiledGrammar compiled_grammar = compiler.CompileGrammar(grammar);
    Grammar compiled = compiled_grammar.GetGrammar();
    constrained_rule = -1;
    for (int32_t rule_id = 0; rule_id < compiled->NumRules(); ++rule_id) {
      if (compiled->GetRule(rule_id).json_number_multiple_of_coefficient > 0) {
        constrained_rule = rule_id;
        break;
      }
    }
    ASSERT_GE(constrained_rule, 0) << "cache_enabled=" << cache_enabled;
    EXPECT_EQ(compiled->GetRule(constrained_rule).json_number_multiple_of_coefficient, 25);
    EXPECT_EQ(compiled->GetRule(constrained_rule).json_number_multiple_of_decimal_scale, 2);
    GrammarMatcher accepted_matcher(
        compiled_grammar, std::nullopt, /*terminate_without_stop_token=*/true
    );
    EXPECT_TRUE(accepted_matcher.AcceptString("0.25"));
    GrammarMatcher rejected_matcher(
        compiled_grammar, std::nullopt, /*terminate_without_stop_token=*/true
    );
    EXPECT_TRUE(rejected_matcher.AcceptString("0.1"));
    EXPECT_FALSE(rejected_matcher.IsTerminated());
  }
}
