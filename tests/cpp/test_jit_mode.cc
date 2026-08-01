#include <gtest/gtest.h>

#include <future>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "compiled_grammar_impl.h"
#include "grammar_functor.h"
#include "xgrammar/compiler.h"
#include "xgrammar/tokenizer_info.h"

namespace xgrammar {
namespace {

TEST(JitModeTest, PreservesRepetitionRangesOnlyInJitMode) {
  TokenizerInfo tokenizer_info(
      {">", "<", "a", "aa", "b"}, VocabType::RAW, std::nullopt, std::vector<int32_t>{}
  );
  GrammarCompiler eager_compiler(
      tokenizer_info,
      /*max_threads=*/1,
      /*cache_enabled=*/false,
      /*max_memory_bytes=*/-1,
      /*jit_mode=*/false
  );
  GrammarCompiler jit_compiler(
      tokenizer_info,
      /*max_threads=*/1,
      /*cache_enabled=*/false,
      /*max_memory_bytes=*/-1,
      /*jit_mode=*/true
  );
  const auto grammar = R"(root ::= ">" [a-z]{63,65} "<")";
  const CompiledGrammar eager_grammar = eager_compiler.CompileGrammar(grammar);
  const CompiledGrammar jit_grammar = jit_compiler.CompileGrammar(grammar);
  const auto contains_repetition_range = [](const CompiledGrammar& compiled_grammar) {
    for (int32_t index = 0; index < compiled_grammar->grammar->NumGrammarExprs(); ++index) {
      const auto expression = compiled_grammar->grammar->GetGrammarExpr(index);
      if (expression.type == Grammar::Impl::GrammarExprType::kRepeat) {
        return true;
      }
    }
    return false;
  };

  EXPECT_FALSE(contains_repetition_range(eager_grammar));
  EXPECT_TRUE(contains_repetition_range(jit_grammar));
}

TEST(JitModeTest, RepeatedCharacterClassMasksAreCached) {
  TokenizerInfo tokenizer_info(
      {"a", "ab", "abc", "ab<", "<", std::string("\xc3", 1)},
      VocabType::RAW,
      std::nullopt,
      std::vector<int32_t>{}
  );
  GrammarCompiler compiler(
      tokenizer_info,
      /*max_threads=*/1,
      /*cache_enabled=*/false,
      /*max_memory_bytes=*/-1,
      /*jit_mode=*/true
  );
  CompiledGrammar compiled_grammar = compiler.CompileGrammar(R"(root ::= [a-z]{1,4} "<")");

  int32_t character_class_expr_id = -1;
  for (int32_t index = 0; index < compiled_grammar->grammar->NumGrammarExprs(); ++index) {
    const auto expression = compiled_grammar->grammar->GetGrammarExpr(index);
    if (expression.type != Grammar::Impl::GrammarExprType::kRepeat) {
      continue;
    }
    const auto& repeated_rule = compiled_grammar->grammar->GetRule(expression[0]);
    const auto choices = compiled_grammar->grammar->GetGrammarExpr(repeated_rule.body_expr_id);
    ASSERT_EQ(choices.type, Grammar::Impl::GrammarExprType::kChoices);
    ASSERT_EQ(choices.size(), 1);
    const auto sequence = compiled_grammar->grammar->GetGrammarExpr(choices[0]);
    ASSERT_EQ(sequence.type, Grammar::Impl::GrammarExprType::kSequence);
    ASSERT_EQ(sequence.size(), 1);
    character_class_expr_id = sequence[0];
    break;
  }
  ASSERT_NE(character_class_expr_id, -1);

  constexpr int kThreadCount = 16;
  std::vector<const AdaptiveTokenMask*> masks(kThreadCount);
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int index = 0; index < kThreadCount; ++index) {
    threads.emplace_back([&, index] {
      masks[index] =
          &compiled_grammar->GetRepeatedCharacterClassTokenMask(character_class_expr_id, -1);
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_NE(masks[0], nullptr);
  for (const AdaptiveTokenMask* mask : masks) {
    EXPECT_EQ(mask, masks[0]);
  }
  const auto* bounded_mask =
      &compiled_grammar->GetRepeatedCharacterClassTokenMask(character_class_expr_id, 2);
  EXPECT_NE(bounded_mask, masks[0]);
  EXPECT_EQ(compiled_grammar->character_class_token_summaries.size(), 1);
  EXPECT_EQ(compiled_grammar->repeated_character_class_token_masks.size(), 2);
}

TEST(JitModeTest, RuleMasksAreSharedOnlyWithoutRuntimeContext) {
  TokenizerInfo tokenizer_info(
      {"\"", "a", "ab", "b", "x", "y"}, VocabType::RAW, std::nullopt, std::vector<int32_t>{}
  );
  GrammarCompiler compiler(
      tokenizer_info,
      /*max_threads=*/1,
      /*cache_enabled=*/true,
      /*max_memory_bytes=*/-1,
      /*jit_mode=*/true
  );
  const auto first = compiler.CompileGrammar(
      "root ::= \"\\\"\" content \"\\\"x\"\n"
      "content ::= \"\" | [a-z] content"
  );
  const auto second = compiler.CompileGrammar(
      "root ::= \"\\\"\" content \"\\\"y\"\n"
      "content ::= \"\" | [a-z] content"
  );
  ASSERT_NE(first->rule_level_cache, nullptr);
  ASSERT_NE(second->rule_level_cache, nullptr);
  EXPECT_EQ(first->rule_level_cache->ImplPtr(), second->rule_level_cache->ImplPtr());

  const auto generate_rule_masks = [](CompiledGrammar compiled_grammar,
                                      const std::string& rule_name) {
    int32_t rule_id = -1;
    for (int32_t index = 0; index < compiled_grammar->grammar->NumRules(); ++index) {
      if (compiled_grammar->grammar->GetRule(index).name == rule_name) {
        rule_id = index;
        break;
      }
    }
    ASSERT_NE(rule_id, -1);
    const auto& rule = compiled_grammar->grammar->GetRule(rule_id);
    const auto& rule_fsm = compiled_grammar->grammar->per_rule_fsms[rule_id];
    ASSERT_TRUE(rule_fsm.has_value());
    std::unordered_set<int> reachable_states;
    rule_fsm->GetFsm().GetReachableStates(&reachable_states);
    for (int32_t element_id : reachable_states) {
      if (!rule_fsm->GetFsm().IsScanableState(element_id)) {
        continue;
      }
      compiled_grammar->GetAdaptiveTokenMask(
          ParserState(rule_id, rule.body_expr_id, element_id, ParserState::kNoPrevInputPos, 0),
          /*is_root_rule=*/false
      );
    }
  };

  const std::size_t initial_size = MemorySize(*first->rule_level_cache);
  generate_rule_masks(first, "content");
  const std::size_t populated_size = MemorySize(*first->rule_level_cache);
  EXPECT_GT(populated_size, initial_size);
  generate_rule_masks(second, "content");
  EXPECT_EQ(MemorySize(*first->rule_level_cache), populated_size);

  const auto context_dependent = compiler.CompileGrammar(
      "root ::= limited\n"
      "limited[max_tokens=1] ::= \"\" | [a-z] limited"
  );
  generate_rule_masks(context_dependent, "limited");
  EXPECT_EQ(MemorySize(*first->rule_level_cache), populated_size);
}

TEST(JitModeTest, ConcurrentGenerationReusesOneMask) {
  std::vector<std::string> vocabulary;
  for (int value = 32; value < 127; ++value) {
    vocabulary.emplace_back(1, static_cast<char>(value));
  }
  vocabulary.insert(vocabulary.end(), {"alpha", "beta", "gamma", "delta"});
  TokenizerInfo tokenizer_info(vocabulary, VocabType::RAW, std::nullopt, std::vector<int32_t>{});
  GrammarCompiler compiler(
      tokenizer_info,
      /*max_threads=*/1,
      /*cache_enabled=*/false,
      /*max_memory_bytes=*/-1,
      /*jit_mode=*/true
  );
  CompiledGrammar compiled_grammar = compiler.CompileGrammar(R"(root ::= [a-z]+ "!")");

  const int32_t root_rule_id = compiled_grammar->grammar->GetRootRuleId();
  const auto root_rule = compiled_grammar->grammar->GetRule(root_rule_id);
  const auto& root_fsm = compiled_grammar->grammar->per_rule_fsms[root_rule_id]->GetFsm();
  std::unordered_set<int> reachable_states;
  root_fsm.GetReachableStates(&reachable_states);
  int32_t scanable_state_id = -1;
  for (int32_t state_id : reachable_states) {
    if (root_fsm.IsScanableState(state_id)) {
      scanable_state_id = state_id;
      break;
    }
  }
  ASSERT_NE(scanable_state_id, -1);

  const ParserState state(
      root_rule_id, root_rule.body_expr_id, scanable_state_id, ParserState::kNoPrevInputPos
  );
  constexpr int kThreadCount = 32;
  std::vector<const AdaptiveTokenMask*> generated_masks(kThreadCount);
  std::promise<void> start_promise;
  std::shared_future<void> start_signal = start_promise.get_future().share();
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (int index = 0; index < kThreadCount; ++index) {
    threads.emplace_back([&, index] {
      start_signal.wait();
      generated_masks[index] = &compiled_grammar->GetAdaptiveTokenMask(state, true);
    });
  }

  start_promise.set_value();
  for (auto& thread : threads) {
    thread.join();
  }

  ASSERT_NE(generated_masks[0], nullptr);
  for (const AdaptiveTokenMask* mask : generated_masks) {
    EXPECT_EQ(mask, generated_masks[0]);
  }
  EXPECT_EQ(compiled_grammar->adaptive_token_mask_cache.size(), 1);
}

}  // namespace
}  // namespace xgrammar
