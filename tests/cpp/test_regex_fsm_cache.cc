/**
 * \file tests/cpp/test_regex_fsm_cache.cc
 * \brief Regression tests for regex FSM reuse across JSON Schema compilation stages.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>

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
